
//===-- qlogo/procedures.cpp - Procedures class implementation -------*- C++ -*-===//
//
// Copyright 2017-2024 Jason Sikes
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted under the conditions specified in the
// license found in the LICENSE file in the project root.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the implementation of the Procedures class, which is
/// responsible for for organizing all procedures in QLogo: primitives,
/// user-defined, and library.
///
//===----------------------------------------------------------------------===//

#include "compiler.h"
#include "workspace/procedures.h"
#include "datum.h"
#include "kernel.h"
#include "flowcontrol.h"
#include "astnode.h"
#include "workspace/callframe.h"
#include "sharedconstants.h"
#include <QDateTime>
#include "QApplication"
#include <QObject>


Procedures::Procedures() {
    Config::get().setMainProcedures(this);
    lastProcedureCreatedTimestamp = QDateTime::currentMSecsSinceEpoch();

    // The procedure table code is generated by util/generate_command_table.py:
#include "workspace/primitivetable.h"

}

Procedures::~Procedures() {
    Config::get().setMainProcedures(nullptr);
}

void Procedures::defineProcedure(DatumPtr cmd, DatumPtr procnameP, DatumPtr text,
                             DatumPtr sourceText) {
    procnameP.wordValue()->numberValue();
    if (procnameP.wordValue()->numberIsValid)
        throw FCError::doesntLike(cmd, procnameP);

    QString procname = procnameP.wordValue()->keyValue();

    QChar firstChar = (procname)[0];
    if ((firstChar == '"') || (firstChar == ':'))
        throw FCError::doesntLike(cmd, procnameP);

    if (stringToCmd.contains(procname))
        throw FCError::isPrimitive(procnameP);

    DatumPtr procBody = createProcedure(cmd, text, sourceText);

    procedures[procname] = procBody;

    // TODO: Is this an appropriate place to unbury a procedure?
}

DatumPtr Procedures::createProcedure(DatumPtr cmd, DatumPtr text, DatumPtr sourceText) {
    Procedure *body = new Procedure();
    DatumPtr bodyP(body);

    lastProcedureCreatedTimestamp = QDateTime::currentMSecsSinceEpoch();

    QString cmdString = cmd.wordValue()->keyValue();
    bool isMacro = ((cmdString == QObject::tr(".MACRO")) || (cmdString == QObject::tr(".DEFMACRO")));

    body->countOfDefaultParams = 0;
    body->countOfMinParams = 0;
    body->countOfMaxParams = 0;
    body->isMacro = isMacro;
    body->sourceText = sourceText;

    bool isOptionalDefined = false;
    bool isRestDefined = false;
    bool isDefaultDefined = false;

    // Parameters are defined in the following format, and are processed in the following order:
    // Required Inputs, e.g. :FOO
    // Optional inputs, e.g. [:BAZ 87]
    // Rest input, e.g. [:GARPLY]
    // Default number, e.g. 5

    ListIterator paramIter = text.listValue()->head.listValue()->newIterator();

    while (paramIter.elementExists()) {
        DatumPtr currentParam = paramIter.element();

        if (currentParam.isWord()) { // This is a default number, or a required input.
            double paramAsNumber = currentParam.wordValue()->numberValue();
            if (currentParam.wordValue()->numberIsValid)
            { // This is a default number, e.g. 5
                if (isDefaultDefined)
                    throw FCError::doesntLike(cmd, currentParam);
                if ((paramAsNumber != floor(paramAsNumber)) ||
                    (paramAsNumber < body->countOfMinParams) ||
                    ((paramAsNumber > body->countOfMaxParams) &&
                     (body->countOfMaxParams >= 0)))
                    throw FCError::doesntLike(cmd, currentParam);
                body->countOfDefaultParams = paramAsNumber;
                isDefaultDefined = true;
            }
            else
            { // This is a required input, e.g. :FOO
                if (isDefaultDefined || isRestDefined || isOptionalDefined)
                    throw FCError::doesntLike(cmd, currentParam);
                QString paramName =
                    currentParam.wordValue()->keyValue();
                if (paramName.startsWith(':') || paramName.startsWith('"'))
                    paramName.remove(0, 1);
                if (paramName.size() < 1)
                    throw FCError::doesntLike(cmd, currentParam);
                body->requiredInputs.append(paramName);
                body->countOfDefaultParams += 1;
                body->countOfMinParams += 1;
                body->countOfMaxParams += 1;
            }
        } else if (currentParam.isList()) { // This is an optional input or a rest input.
            List *paramList = currentParam.listValue();

            if (paramList->isEmpty())
                throw FCError::doesntLike(cmd, currentParam);

            if (paramList->count() == 1) { // This is a rest input, e.g. [:GARPLY]
                if (isRestDefined)
                    throw FCError::doesntLike(cmd, currentParam);
                DatumPtr param = paramList->head;
                if (param.isWord()) {
                    QString restName = param.wordValue()->keyValue();
                    if (restName.startsWith(':') || restName.startsWith('"'))
                        restName.remove(0, 1);
                    if (restName.size() < 1)
                        throw FCError::doesntLike(cmd, param);
                    body->restInput = restName;
                    isRestDefined = true;
                    body->countOfMaxParams = -1;
                } else {
                    throw FCError::doesntLike(cmd, param);
                }
            } else { // This is an optional input, e.g. [:BAZ 87]
                if (isRestDefined || isDefaultDefined)
                    throw FCError::doesntLike(cmd, currentParam);
                DatumPtr param = paramList->head;
                if (param.isWord()) {
                    QString name = param.wordValue()->keyValue();
                    if (name.startsWith(':') || name.startsWith('"'))
                        name.remove(0, 1);
                    if (name.size() < 1)
                        throw FCError::doesntLike(cmd, param);
                    body->optionalInputs.append(name);
                    body->optionalDefaults.append(paramList);
                    isOptionalDefined = true;
                    body->countOfMaxParams += 1;
                } else {
                    throw FCError::doesntLike(cmd, param);
                }
            } // endif optional or rest input
        } else {
            throw FCError::doesntLike(cmd, currentParam);
        }
    } // /for each parameter

    body->instructionList = text.listValue()->tail;
    if (body->instructionList.isNothing())
        body->instructionList = DatumPtr(new List);

    // Iterate over the instruction list and add tags to the tagToLine map.
    ListIterator lineIter = body->instructionList.listValue()->newIterator();
    while (lineIter.elementExists()) {
        DatumPtr lineP = lineIter.element();
        ListIterator wordIter = lineP.listValue()->newIterator();
        while (wordIter.elementExists()) {
            DatumPtr d = wordIter.element();
            if (d.isWord() && (d.wordValue()->keyValue() == QObject::tr("TAG")) &&
                wordIter.elementExists()) {
                DatumPtr d = wordIter.element();
                if (d.isWord()) {
                    QString param = d.wordValue()->keyValue();
                    if ((param.size() > 1) && (param)[0] == '"') {
                        QString tag = param.right(param.size() - 1);
                        body->tagToLine[tag] = lineP;
                    }
                }
            }
        }
    }
    return bodyP;
}

void Procedures::copyProcedure(DatumPtr newnameP, DatumPtr oldnameP) {
    lastProcedureCreatedTimestamp = QDateTime::currentMSecsSinceEpoch();
    QString newname = newnameP.wordValue()->keyValue();
    QString oldname = oldnameP.wordValue()->keyValue();

    if (stringToCmd.contains(newname))
        throw FCError::isPrimitive(newnameP);

    if (stringToCmd.contains(oldname)) {
        throw FCError::isPrimitive(oldnameP);
    }
    if (isNamedProcedure(oldname)) {
        procedures[newname] = procedures[oldname];
        return;
    }
    throw FCError::noHow(oldnameP);
}

void Procedures::eraseProcedure(DatumPtr procnameP) {
    lastProcedureCreatedTimestamp = QDateTime::currentMSecsSinceEpoch();

    QString procname = procnameP.wordValue()->keyValue();
    if (stringToCmd.contains(procname))
        throw FCError::isPrimitive(procnameP);
    procedures.remove(procname);
}

DatumPtr Procedures::procedureText(DatumPtr procnameP) {
    QString procname = procnameP.wordValue()->keyValue();

    if (stringToCmd.contains(procname))
        throw FCError::isPrimitive(procnameP);
    if ( ! isNamedProcedure(procname))
        throw FCError::noHow(procnameP);
    Procedure *body = procedureForName(procname).procedureValue();

    List *retval = new List();
    ListBuilder retvalBuilder(retval);

    List *inputs = new List();
    ListBuilder inputsBuilder(inputs);

    // Generate the parameters interface.
    for (auto &i : body->requiredInputs) {
        inputsBuilder.append(DatumPtr(i));
    }

    QList<DatumPtr>::iterator d = body->optionalDefaults.begin();
    for (auto &i : body->optionalInputs) {
        List *optInput = d->listValue()->tail.listValue();
        ++d;
        inputsBuilder.append(new List(DatumPtr(i),optInput));
    }

    if (body->restInput != "") {
        List *restInput = new List();
        restInput = new List(DatumPtr(body->restInput), restInput);
        inputsBuilder.append(DatumPtr(restInput));
    }

    if (body->countOfDefaultParams != body->requiredInputs.size()) {
        inputsBuilder.append(DatumPtr(body->countOfDefaultParams));
    }

    retvalBuilder.append(DatumPtr(inputs));

    // Generate and append the instruction list.
    ListIterator b = body->instructionList.listValue()->newIterator();

    while (b.elementExists()) {
        retvalBuilder.append(b.element());
    }

    return DatumPtr(retval);
}

DatumPtr Procedures::procedureFulltext(DatumPtr procnameP, bool shouldValidate) {
    const QString procname = procnameP.wordValue()->keyValue();
    if (stringToCmd.contains(procname))
        throw FCError::isPrimitive(procnameP);

    if (isNamedProcedure(procname)) {
        Procedure *body = procedureForName(procname).procedureValue();

        // If there is no source text, generate it from the instruction list.
        if (body->sourceText == nothing) {
            List *retval = new List();
            ListBuilder retvalBuilder(retval);
            retvalBuilder.append(DatumPtr(procedureTitle(procnameP)));

            ListIterator b = body->instructionList.listValue()->newIterator();

            while (b.elementExists()) {
                retvalBuilder.append(DatumPtr(unreadList(b.element().listValue(), false)));
            }

            DatumPtr end(QObject::tr("END"));
            retvalBuilder.append(end);
            return DatumPtr(retval);
        } else {
            return body->sourceText;
        }
    } else if (shouldValidate) {
        throw FCError::noHow(procnameP);
    }

    // If there is no procedure by that name, generate an empty procedure.
    List *retval = new List();
    ListBuilder retvalBuilder(retval);
    retvalBuilder.append(
        DatumPtr(QObject::tr("to ") + procnameP.wordValue()->printValue()));
    retvalBuilder.append(DatumPtr(QObject::tr("END")));
    return DatumPtr(retval);
}

QString Procedures::procedureTitle(DatumPtr procnameP) {
    QString procname = procnameP.wordValue()->keyValue();

    if (stringToCmd.contains(procname))
        throw FCError::isPrimitive(procnameP);
    if ( ! isNamedProcedure(procname))
        throw FCError::noHow(procnameP);

    Procedure *body = procedureForName(procname).procedureValue();

    DatumPtr firstlineP = DatumPtr(new List());

    List *firstLine = firstlineP.listValue();
    ListBuilder firstLineBuilder(firstLine);

    if (body->isMacro)
        firstLineBuilder.append(DatumPtr(QObject::tr(".macro")));
    else
        firstLineBuilder.append(DatumPtr(QObject::tr("to")));
    firstLineBuilder.append(procnameP);

    QString paramName;

    for (auto &i : body->requiredInputs) {
        paramName = i;
        paramName.prepend(':');
        firstLineBuilder.append(DatumPtr(paramName));
    }

    for (auto &i : body->optionalDefaults) {
        firstLineBuilder.append(i);
    }

    paramName = body->restInput;
    if (paramName != "") {
        paramName.push_front(':');
        List *restInput = new List();
        restInput = new List(DatumPtr(paramName), restInput);
        firstLineBuilder.append(DatumPtr(restInput));
    }

    if (body->countOfDefaultParams != body->requiredInputs.size()) {
        firstLineBuilder.append(DatumPtr(body->countOfDefaultParams));
    }

    QString retval = unreadList(firstLine, false);
    return retval;
}

DatumPtr Procedures::procedureForName(QString aName)
{
    if ( ! procedures.contains(aName)) {
        if ( ! stdLib.allProcedureNames().contains(aName)) {
            return nothing;
        }
        // TODO: Procedure is in our library, add it to our procedures table.
    }
    Q_ASSERT(procedures.contains(aName));
    return procedures[aName];
}


bool Procedures::isNamedProcedure(QString aName)
{
    return procedures.contains(aName)
        || stdLib.allProcedureNames().contains(aName);
}


DatumPtr Procedures::astnodeFromPrimitive(DatumPtr cmdP, int &minParams, int &defaultParams, int &maxParams) {
    QString cmdString = cmdP.wordValue()->keyValue();
    DatumPtr node = DatumPtr(new ASTNode(cmdP));
    Cmd_t command = stringToCmd[cmdString];
    defaultParams = command.countOfDefaultParams;
    minParams = command.countOfMinParams;
    maxParams = command.countOfMaxParams;
    node.astnodeValue()->genExpression = command.method;
    node.astnodeValue()->returnType = command.returnType;
    return node;
}


DatumPtr Procedures::astnodeFromProcedure(DatumPtr cmdP, int &minParams, int &defaultParams, int &maxParams) {
    QString cmdString = cmdP.wordValue()->keyValue();
    DatumPtr procBody = procedureForName(cmdString);
    DatumPtr node = DatumPtr(new ASTNode(cmdP));
    // if (procBody.procedureValue()->isMacro)
    //     node.astnodeValue()->kernel = &Kernel::executeMacro;
    // else
    //     node.astnodeValue()->kernel = &Kernel::executeProcedure;

    node.astnodeValue()->genExpression  = &Compiler::genExecProcedure;
    node.astnodeValue()->returnType = RequestReturnDatum;
    node.astnodeValue()->procedure = procBody;
    defaultParams = procBody.procedureValue()->countOfDefaultParams;
    minParams = procBody.procedureValue()->countOfMinParams;
    maxParams = procBody.procedureValue()->countOfMaxParams;
    return node;
}


DatumPtr Procedures::astnodeFromCommand(DatumPtr cmdP, int &minParams,
                                    int &defaultParams, int &maxParams) {
    QString cmdString = cmdP.wordValue()->keyValue();

    if (stringToCmd.contains(cmdString)) { // This is a primitive.
        return astnodeFromPrimitive(cmdP, minParams, defaultParams, maxParams);
    } else if (isNamedProcedure(cmdString)) { // This is a procedure.
        return astnodeFromProcedure(cmdP, minParams, defaultParams, maxParams);
        // TODO: Foo and setFoo
    } else { // This is not a command.
        throw FCError::noHow(cmdP);
    }
    return nothing;
}


DatumPtr Procedures::astnodeWithLiterals(DatumPtr cmd, DatumPtr params) {
    int minParams = 0, maxParams = 0, defaultParams = 0;
    DatumPtr node = astnodeFromCommand(cmd, minParams, defaultParams, maxParams);

    int countOfChildren = params.listValue()->count();
    if (countOfChildren < minParams)
        throw FCError::notEnoughInputs(cmd);
    if ((countOfChildren > maxParams) && (maxParams != -1))
        throw FCError::tooManyInputs(cmd);

    ListIterator iter = params.listValue()->newIterator();
    while (iter.elementExists()) {
        DatumPtr p = iter.element();
        DatumPtr a = DatumPtr(new ASTNode(QObject::tr("literal")));
        // TODO: What is the ReturnType of this?
        a.astnodeValue()->genExpression = &Compiler::genLiteral;
        a.astnodeValue()->addChild(p);
        node.astnodeValue()->addChild(a);
    }
    return node;
}

bool Procedures::isProcedure(QString procname) {
    if (stringToCmd.contains(procname)
        || procedures.contains(procname))
        return true;
    return false;
}

bool Procedures::isMacro(QString procname) {
    if (procedures.contains(procname)) {
        DatumPtr procedure = procedures[procname];
        return procedure.procedureValue()->isMacro;
    }
    return false;
}

bool Procedures::isPrimitive(QString procname) {
    return (stringToCmd.contains(procname));
}

bool Procedures::isDefined(QString procname) {
    return (procedures.contains(procname));
}

DatumPtr Procedures::allProcedureNames() {
    List *retval = new List();
    ListBuilder retvalBuilder(retval);
    for (const auto &iter : procedures.asKeyValueRange()) {
        retvalBuilder.append(DatumPtr(iter.first));
    }
    return DatumPtr(retval);
}

DatumPtr Procedures::allPrimitiveProcedureNames() {
    List *retval = new List();
    ListBuilder retvalBuilder(retval);
    for (const auto &iter : stringToCmd.asKeyValueRange()) {
        retvalBuilder.append(DatumPtr(iter.first));
    }
    return DatumPtr(retval);
}

DatumPtr Procedures::arity(DatumPtr nameP) {
    int minParams, defParams, maxParams;
    QString procname = nameP.wordValue()->keyValue();

    if (procedures.contains(procname)) {
        DatumPtr command = procedures[procname];
        minParams = command.procedureValue()->countOfMinParams;
        defParams = command.procedureValue()->countOfDefaultParams;
        maxParams = command.procedureValue()->countOfMaxParams;
    } else if (stringToCmd.contains(procname)) {
        Cmd_t command = stringToCmd[procname];
        minParams = command.countOfMinParams;
        defParams = command.countOfDefaultParams;
        maxParams = command.countOfMaxParams;
    } else {
        throw FCError::noHow(nameP);
        return nothing;
    }

    List *retval = new List();
    ListBuilder retvalBuilder(retval);
    retvalBuilder.append(DatumPtr(minParams));
    retvalBuilder.append(DatumPtr(defParams));
    retvalBuilder.append(DatumPtr(maxParams));
    return DatumPtr(retval);
}

QString Procedures::unreadDatum(DatumPtr aDatum, bool isInList) {
    switch (aDatum.isa()) {
    case Datum::typeWord:
        return unreadWord(aDatum.wordValue(), isInList);
        break;
    case Datum::typeList:
        return unreadList(aDatum.listValue(), isInList);
    case Datum::typeArray:
        return unreadArray(aDatum.arrayValue());
    default:
        Q_ASSERT(false);
    }
    return "";
}

QString Procedures::unreadList(List *aList, bool isInList) {
    QString retval("");
    if (isInList)
        retval = "[";
    ListIterator i = aList->newIterator();
    while (i.elementExists()) {
        DatumPtr e = i.element();
        if ((retval != "[") && (retval != ""))
            retval.append(' ');
        retval.append(unreadDatum(e, true));
    }
    if (isInList)
        retval.append(']');
    return retval;
}

QString Procedures::unreadArray(Array *anArray) {
    QString retval("{");
    for (auto &i : anArray->array) {
        if (retval.size() > 1)
            retval.append(' ');
        retval.append(unreadDatum(i, true));
    }
    retval.append('}');
    return retval;
}

QString Procedures::unreadWord(Word *aWord, bool isInList) {
    aWord->numberValue();
    if (aWord->numberIsValid)
        return aWord->showValue();

    QString retval("");
    if (!isInList)
        retval = "\"";

    const QString src = aWord->showValue();
    if (src.size() == 0)
        return retval + "||";

    if (aWord->isForeverSpecial) {
        retval.append('|');
        for (auto iter = src.begin(); iter != src.end(); ++iter) {
            QChar letter = *iter;
            if ((iter == src.begin()) && (letter == '"')) {
                retval = "\"|";
            } else {
                if (letter == '|') {
                    retval.append('\\');
                }
                retval.append(letter);
            }
        }
        retval.append('|');
    } else {
        for (auto letter : src) {
            if ((letter == ' ') || (letter == '[') || (letter == ']') ||
                (letter == '{') || (letter == '}') || (letter == '|') ||
                (letter == '\n')) {
                retval.append('\\');
            }
            retval.append(letter);
        }
    }
    return retval;
}

QString Procedures::printoutDatum(DatumPtr aDatum) {
    switch (aDatum.isa()) {
    case Datum::typeWord:
        return unreadWord(aDatum.wordValue());
        break;
    case Datum::typeList:
        return unreadList(aDatum.listValue(), true);
    case Datum::typeArray:
        return unreadArray(aDatum.arrayValue());
    default:
        Q_ASSERT(false);
    }
    return "";
}

