/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mongo/db/ops/modifier_pull.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/ops/field_checker.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/db/ops/path_support.h"

namespace mongo {

    namespace mb = mutablebson;

    struct ModifierPull::PreparedState {

        PreparedState(mb::Document& doc)
            : doc(doc)
            , idxFound(0)
            , elemFound(doc.end())
            , boundDollar("")
            , elementsToRemove()
            , noOp(false) {
        }

        // Document that is going to be changed.
        mb::Document& doc;

        // Index in _fieldRef for which an Element exist in the document.
        size_t idxFound;

        // Element corresponding to _fieldRef[0.._idxFound].
        mb::Element elemFound;

        // Value to bind to a $-positional field, if one is provided.
        std::string boundDollar;

        // Values to be removed.
        std::vector<mb::Element> elementsToRemove;

        // True if this update is a no-op
        bool noOp;
    };

    ModifierPull::ModifierPull()
        : ModifierInterface()
        , _fieldRef()
        , _posDollar(0)
        , _exprElt()
        , _exprObj()
        , _matchExpr()
        , _matcherOnPrimitive(false)
        , _preparedState() {
    }

    ModifierPull::~ModifierPull() {
    }

    Status ModifierPull::init(const BSONElement& modExpr) {
        // Perform standard field name and updateable checks.
        _fieldRef.parse(modExpr.fieldName());
        Status status = fieldchecker::isUpdatable(_fieldRef);
        if (! status.isOK()) {
            return status;
        }

        // If a $-positional operator was used, get the index in which it occurred
        // and ensure only one occurrence.
        size_t foundCount;
        bool foundDollar = fieldchecker::isPositional(_fieldRef, &_posDollar, &foundCount);
        if (foundDollar && foundCount > 1) {
            return Status(ErrorCodes::BadValue, "too many positional($) elements found.");
        }

        _exprElt = modExpr;
        if (_exprElt.type() == Object) {

            _exprObj = _exprElt.embeddedObject();
            _matcherOnPrimitive = (_exprObj.firstElement().getGtLtOp() != 0);
            if (_matcherOnPrimitive)
                _exprObj = BSON( "" << _exprObj );

            StatusWithMatchExpression parseResult = MatchExpressionParser::parse(_exprObj);
            if (!parseResult.isOK())
                return parseResult.getStatus();

            _matchExpr.reset(parseResult.getValue());
        }

        return Status::OK();
    }

    Status ModifierPull::prepare(mb::Element root,
                                 const StringData& matchedField,
                                 ExecInfo* execInfo) {

        _preparedState.reset(new PreparedState(root.getDocument()));

        // If we have a $-positional field, it is time to bind it to an actual field part.
        if (_posDollar) {
            if (matchedField.empty()) {
                return Status(ErrorCodes::BadValue, "matched field not provided");
            }
            _preparedState->boundDollar = matchedField.toString();
            _fieldRef.setPart(_posDollar, _preparedState->boundDollar);
        }

        // Locate the field name in 'root'.
        Status status = pathsupport::findLongestPrefix(_fieldRef,
                                                       root,
                                                       &_preparedState->idxFound,
                                                       &_preparedState->elemFound);

        // FindLongestPrefix may say the path does not exist at all, which is fine here, or
        // that the path was not viable or otherwise wrong, in which case, the mod cannot
        // proceed.
        if (status.code() == ErrorCodes::NonExistentPath) {
            _preparedState->elemFound = root.getDocument().end();
        } else if (!status.isOK()) {
            return status;
        }

        // We register interest in the field name. The driver needs this info to sort out if
        // there is any conflict among mods.
        execInfo->fieldRef[0] = &_fieldRef;

        if (!_preparedState->elemFound.ok() ||
            _preparedState->idxFound < (_fieldRef.numParts() - 1)) {
            // If no target element exists, then there is nothing to do here.
            _preparedState->noOp = execInfo->noOp = true;
            return Status::OK();
        }

        // This operation only applies to arrays
        if (_preparedState->elemFound.getType() != mongo::Array)
            return Status(
                ErrorCodes::BadValue,
                "Cannot apply $pull to a non-array value");

        // If the array is empty, there is nothing to pull, so this is a noop.
        if (!_preparedState->elemFound.hasChildren()) {
            _preparedState->noOp = execInfo->noOp = true;
            return Status::OK();
        }

        // Walk the values in the array
        mb::Element cursor = _preparedState->elemFound.leftChild();
        while (cursor.ok()) {
            if (isMatch(cursor))
                _preparedState->elementsToRemove.push_back(cursor);
            cursor = cursor.rightSibling();
        }

        // If we didn't find any elements to add, then this is a no-op, and therefore in place.
        if (_preparedState->elementsToRemove.empty()) {
            _preparedState->noOp = execInfo->noOp = true;
        }

        return Status::OK();
    }

    Status ModifierPull::apply() const {
        dassert(_preparedState->noOp == false);

        dassert(_preparedState->elemFound.ok() &&
                _preparedState->idxFound == (_fieldRef.numParts() - 1));

        std::vector<mb::Element>::const_iterator where = _preparedState->elementsToRemove.begin();
        const std::vector<mb::Element>::const_iterator end = _preparedState->elementsToRemove.end();
        for ( ; where != end; ++where)
            const_cast<mb::Element&>(*where).remove();

        return Status::OK();
    }

    Status ModifierPull::log(LogBuilder* logBuilder) const {

        mb::Document& doc = logBuilder->getDocument();

        if (!_preparedState->elemFound.ok() ||
            _preparedState->idxFound < (_fieldRef.numParts() - 1)) {

            // If we didn't find the element that we wanted to pull from, we log an unset for
            // that element.

            mb::Element logElement = doc.makeElementInt(_fieldRef.dottedField(), 1);
            if (!logElement.ok())
                return Status(ErrorCodes::InternalError,
                              "cannot create log entry for $pull mod");

            return logBuilder->addToUnsets(logElement);

        } else {

            // TODO: This is copied more or less identically from $push. As a result, it copies the
            // behavior in $push that relies on 'apply' having been called unless this is a no-op.

            // TODO We can log just a positional unset in several cases. For now, let's just log
            // the full resulting array.

            // We'd like to create an entry such as {$set: {<fieldname>: [<resulting aray>]}} under
            // 'logRoot'.  We start by creating the {$set: ...} Element.

            // Then we create the {<fieldname>:[]} Element, that is, an empty array.
            mb::Element logElement = doc.makeElementArray(_fieldRef.dottedField());
            if (!logElement.ok()) {
                return Status(ErrorCodes::InternalError, "cannot create details for $pull mod");
            }

            mb::Element curr = _preparedState->elemFound.leftChild();
            while (curr.ok()) {

                dassert(curr.hasValue());

                // We need to copy each array entry from the resulting document to the log
                // document.
                mb::Element currCopy = doc.makeElementWithNewFieldName(
                    StringData(),
                    curr.getValue());
                if (!currCopy.ok()) {
                    return Status(ErrorCodes::InternalError, "could create copy element");
                }
                Status status = logElement.pushBack(currCopy);
                if (!status.isOK()) {
                    return Status(ErrorCodes::BadValue, "could not append entry for $pull log");
                }
                curr = curr.rightSibling();
            }

            return logBuilder->addToSets(logElement);
        }
    }

    bool ModifierPull::isMatch(mutablebson::ConstElement element) {

        // TODO: We are assuming that 'element' hasValue is true. That might be OK if the
        // conflict detection logic will prevent us from ever seeing a deserialized element,
        // but are we sure about that?

        dassert(element.hasValue());

        if (!_matchExpr)
            return (element.compareWithBSONElement(_exprElt, false) == 0);

        if (_matcherOnPrimitive) {
            // TODO: This is kinda slow.
            BSONObj candidate = element.getValue().wrap("");
            return _matchExpr->matchesBSON(candidate);
        }

        if (element.getType() != Object)
            return false;

        return _matchExpr->matchesBSON(element.getValueObject());
    }

} // namespace mongo
