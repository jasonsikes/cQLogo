//===-- parser.cpp - Parser class implementation -------*- C++ -*-===//
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
/// This file contains the implementation of the Parser class, which is
/// responsible for parsing text, lists, and arrays.
///
//===----------------------------------------------------------------------===//

#include "compiler.h"
#include "parser.h"
#include "astnode.h"
#include "controller/logocontroller.h"
#include "datum.h"
#include "flowcontrol.h"
#include "kernel.h"
#include "runparser.h"
#include <qdatetime.h>
#include <qdebug.h>
#include "workspace/procedures.h"
#include "controller/textstream.h"
// TODO: we could reimplement this into something a little faster.
const QString specialChars("+-()*%/<>=");

QHash<List*, QList<QList<DatumPtr>>> Parser::astListTable;

bool isTag(DatumPtr node)
{
    return node.astnodeValue()->genExpression == &Compiler::genTag;
}

void Parser::inputProcedure(ASTNode *node, TextStream *readStream)
{
    // to is the command name that initiated inputProcedure(), it is the first
    // word in the input line, 'TO' or '.MACRO'.
    DatumPtr to = node->nodeName;
    if (node->countOfChildren() == 0)
        throw FCError::notEnoughInputs(to);

    // procnameP is the name of the procedure, the second word in the input line,
    // following 'TO' or '.MACRO'.
    DatumPtr procnameP = node->childAtIndex(0);
    if (!procnameP.isWord())
        throw FCError::doesntLike(to, procnameP);

    procnameP.wordValue()->numberValue();
    if (procnameP.wordValue()->numberIsValid)
        throw FCError::doesntLike(to, procnameP);

    QString procname = procnameP.wordValue()->keyValue();

    QChar firstChar = (procname)[0];
    if ((firstChar == '"') || (firstChar == ':') || (firstChar == '(') || (firstChar == ')'))
        throw FCError::doesntLike(to, procnameP);

    if (Config::get().mainProcedures()->isProcedure(procname))
        throw FCError::procDefined(procnameP);

    // Assign the procedure's parameter names and default values.
    DatumPtr textP = DatumPtr(new List());
    ListBuilder textBuilder(textP.listValue());
    DatumPtr firstLine = DatumPtr(new List());
    ListBuilder firstLineBuilder(firstLine.listValue());
    for (int i = 1; i < node->countOfChildren(); ++i)
    {
        firstLineBuilder.append(node->childAtIndex(i));
    }
    textBuilder.append(firstLine);

    // Now read in the body
    forever
    {
        DatumPtr line = readStream->readlistWithPrompt("> ", true, true);
        if (!line.isList()) // this must be the end of the input
            break;
        if (line.listValue()->isEmpty())
            continue;
        DatumPtr first = line.listValue()->head;
        if (first.isWord())
        {
            QString firstWord = first.wordValue()->keyValue();
            if (firstWord == QObject::tr("END"))
                break;
        }
        textBuilder.append(line);
    }

    // The sourcetext is the raw text from which the procedure was defined.
    // We save it in case user executes `FULLTEXT`.
    DatumPtr sourceText = readStream->recentHistory();
    Config::get().mainProcedures()->defineProcedure(to, procnameP, textP, sourceText);

    QString message = QObject::tr("%1 defined\n");
    message = message.arg(procnameP.wordValue()->printValue());
    Config::get().mainKernel()->sysPrint(message);
}


QList<QList<DatumPtr>> Parser::astFromList(List *aList)
{
    QList<QList<DatumPtr>> &retval = astListTable[aList];
    if (aList->astParseTimeStamp <= Config::get().mainProcedures()->timeOfLastProcedureCreation())
    {
        aList->astParseTimeStamp = QDateTime::currentMSecsSinceEpoch();

        DatumPtr runParsedList = runparse(aList);

        listIter = runParsedList.listValue()->newIterator();
        retval.clear();
        QList<DatumPtr> astFlatList;

        advanceToken();

        try
        {
            while (currentToken != nothing)
            {
                astFlatList.push_back(parseRootExp());
            }
        }
        catch (FCError *e)
        {
            // The AST is invalid, so destroy it and rethrow the error.
            destroyAstForList(aList);
            aList->astParseTimeStamp = 0;
            throw e;
        }
        // If the last ASTNode is a tag, generate a NOOP expression at the end
        // to ensure that there is an instruction to jump to.
        if (astFlatList.last().astnodeValue()->genExpression == &Compiler::genTag) {
            ASTNode *noopNode = new ASTNode(DatumPtr(QObject::tr("NOOP")));
            noopNode->genExpression = &Compiler::genNoop;
            noopNode->returnType = RequestReturnNothing;
            astFlatList.append(DatumPtr(noopNode));
        }

        // Now create AST sublists for tag/block pairs.
        QList<DatumPtr> currentBlock;
        for (auto &node : astFlatList)
        {
            if (currentBlock.isEmpty())
                currentBlock.append(node);
            else 
            {
                if (isTag(node) == isTag(currentBlock.last()))
                    currentBlock.append(node);
                else
                {
                    retval.append(currentBlock);
                    currentBlock = {node};
                }
            }
        }
        retval.append(currentBlock);

    }
    return retval;
}

void Parser::destroyAstForList(List *aList)
{
    astListTable.remove(aList);
}

// The remaining methods parse into AST nodes.



DatumPtr Parser::parseRootExp()
{
    DatumPtr node = parseExp();
    if ((currentToken.isa() == Datum::typeWord)
     && (currentToken.wordValue()->keyValue() == QObject::tr("STOP")))
    {
        DatumPtr newNode = DatumPtr(new ASTNode(currentToken));
        newNode.astnodeValue()->genExpression = &Compiler::genStop;
        newNode.astnodeValue()->returnType = RequestReturnNothing;
        newNode.astnodeValue()->addChild(node);
        node = newNode;
        advanceToken();
    }
    return node;
}

DatumPtr Parser::parseExp()
{
    DatumPtr left = parseSumexp();
    while ((currentToken.isa() == Datum::typeWord) &&
           ((currentToken.wordValue()->printValue() == "=") || (currentToken.wordValue()->printValue() == "<>") ||
            (currentToken.wordValue()->printValue() == ">") || (currentToken.wordValue()->printValue() == "<") ||
            (currentToken.wordValue()->printValue() == ">=") || (currentToken.wordValue()->printValue() == "<=")))
    {
        DatumPtr op = currentToken;
        advanceToken();
        DatumPtr right = parseSumexp();

        DatumPtr node = DatumPtr(new ASTNode(op));
        if (right == nothing)
            throw FCError::notEnoughInputs(op);

        if (op.wordValue()->printValue() == "=")
        {
            node.astnodeValue()->genExpression = &Compiler::genEqualp;
            node.astnodeValue()->returnType = RequestReturnBool;
        }
        else if (op.wordValue()->printValue() == "<>")
        {
            node.astnodeValue()->genExpression = &Compiler::genNotequalp;
            node.astnodeValue()->returnType = RequestReturnBool;
        }
        else if (op.wordValue()->printValue() == "<")
        {
            node.astnodeValue()->genExpression = &Compiler::genLessp;
            node.astnodeValue()->returnType = RequestReturnBool;
        }
        else if (op.wordValue()->printValue() == ">")
        {
            node.astnodeValue()->genExpression = &Compiler::genGreaterp;
            node.astnodeValue()->returnType = RequestReturnBool;
        }
        else if (op.wordValue()->printValue() == "<=")
        {
            node.astnodeValue()->genExpression = &Compiler::genLessequalp;
            node.astnodeValue()->returnType = RequestReturnBool;
        }
        else
        {
            node.astnodeValue()->genExpression = &Compiler::genGreaterequalp;
            node.astnodeValue()->returnType = RequestReturnBool;
        }
        node.astnodeValue()->addChild(left);
        node.astnodeValue()->addChild(right);
        left = node;
    }
    return left;
}

DatumPtr Parser::parseSumexp()
{
    DatumPtr left = parseMulexp();
    while ((currentToken.isa() == Datum::typeWord) &&
           ((currentToken.wordValue()->printValue() == "+") || (currentToken.wordValue()->printValue() == "-")))
    {
        DatumPtr op = currentToken;
        advanceToken();
        DatumPtr right = parseMulexp();

        DatumPtr node = DatumPtr(new ASTNode(op));
        if (right == nothing)
            throw FCError::notEnoughInputs(op);

        if (op.wordValue()->printValue() == "+")
        {
            node.astnodeValue()->genExpression = &Compiler::genSum;
            node.astnodeValue()->returnType = RequestReturnReal;
        }
        else
        {
            node.astnodeValue()->genExpression = &Compiler::genDifference;
            node.astnodeValue()->returnType = RequestReturnReal;
        }
        node.astnodeValue()->addChild(left);
        node.astnodeValue()->addChild(right);
        left = node;
    }
    return left;
}

DatumPtr Parser::parseMulexp()
{
    DatumPtr left = parseminusexp();
    while ((currentToken.isa() == Datum::typeWord) &&
           ((currentToken.wordValue()->printValue() == "*") || (currentToken.wordValue()->printValue() == "/") ||
            (currentToken.wordValue()->printValue() == "%")))
    {
        DatumPtr op = currentToken;
        advanceToken();
        DatumPtr right = parseminusexp();

        DatumPtr node = DatumPtr(new ASTNode(op));
        if (right == nothing)
            throw FCError::notEnoughInputs(op);

        if (op.wordValue()->printValue() == "*")
        {
            node.astnodeValue()->genExpression = &Compiler::genProduct;
            node.astnodeValue()->returnType = RequestReturnReal;
        }
        else if (op.wordValue()->printValue() == "/")
        {
            node.astnodeValue()->genExpression = &Compiler::genQuotient;
            node.astnodeValue()->returnType = RequestReturnReal;
        }
        else
        {
            node.astnodeValue()->genExpression = &Compiler::genRemainder;
            node.astnodeValue()->returnType = RequestReturnReal;
        }
        node.astnodeValue()->addChild(left);
        node.astnodeValue()->addChild(right);
        left = node;
    }
    return left;
}

DatumPtr Parser::parseminusexp()
{
    DatumPtr left = parseTermexp();
    while ((currentToken.isa() == Datum::typeWord) && ((currentToken.wordValue()->printValue() == "--")))
    {
        DatumPtr op = currentToken;
        advanceToken();
        DatumPtr right = parseTermexp();

        DatumPtr node = DatumPtr(new ASTNode(op));
        if (right == nothing)
            throw FCError::notEnoughInputs(op);
        
        node.astnodeValue()->genExpression = &Compiler::genDifference;
        node.astnodeValue()->addChild(left);
        node.astnodeValue()->addChild(right);
        left = node;
    }
    return left;
}

DatumPtr Parser::parseTermexp()
{
    if (currentToken == nothing)
        return nothing;

    if (currentToken.isa() == Datum::typeList)
    {
        DatumPtr node(new ASTNode(QObject::tr("List")));
        node.astnodeValue()->genExpression = &Compiler::genLiteral;
        node.astnodeValue()->returnType = RequestReturnDatum;
        node.astnodeValue()->addChild(currentToken);
        advanceToken();
        return node;
    }

    if (currentToken.isa() == Datum::typeArray)
    {
        DatumPtr node(new ASTNode(QObject::tr("Array")));
        node.astnodeValue()->genExpression = &Compiler::genLiteral;
        node.astnodeValue()->returnType = RequestReturnDatum;
        node.astnodeValue()->addChild(currentToken);
        advanceToken();
        return node;
    }

    Q_ASSERT(currentToken.isa() == Datum::typeWord);

    // See if it's an open paren
    if (currentToken.wordValue()->printValue() == "(")
    {
        // This may be an expression or a vararg function
        DatumPtr retval;

        advanceToken();
        if ((currentToken != nothing) && currentToken.isWord())
        {
            QString cmdString = currentToken.wordValue()->keyValue();
            QChar firstChar = (cmdString)[0];
            if ((firstChar != '"') && (firstChar != ':') && ((firstChar < '0') || (firstChar > '9')) &&
                !specialChars.contains(firstChar))
            {
                retval = parseCommand(true);
            }
            else
            {
                retval = parseExp();
            }
        }
        else
        {
            retval = parseExp();
        }

        // Make sure there is a closing paren
        if ((!currentToken.isWord()) || (currentToken.wordValue()->printValue() != ")"))
        {

            throw FCError::parenNf();
        }

        advanceToken();
        return retval;
    }

    QChar firstChar = currentToken.wordValue()->rawValue().at(0);
    if ((firstChar == '"') || (firstChar == ':'))
    {
        QString name = currentToken.wordValue()->rawValue().right(currentToken.wordValue()->rawValue().size() - 1);
        if (!currentToken.wordValue()->isForeverSpecial)
        {
            rawToChar(name);
        }
        if (firstChar == '"')
        {
            DatumPtr node(new ASTNode(QObject::tr("QuotedWord")));
            node.astnodeValue()->genExpression = &Compiler::genLiteral;
            node.astnodeValue()->returnType = RequestReturnDatum;
            node.astnodeValue()->addChild(DatumPtr(DatumPtr(name, currentToken.wordValue()->isForeverSpecial)));
            advanceToken();
            return node;
        }
        else
        {
            DatumPtr node(new ASTNode(QObject::tr("ValueOf")));
            node.astnodeValue()->genExpression = &Compiler::genValueOf;
            node.astnodeValue()->returnType = RequestReturnDatum;
            node.astnodeValue()->addChild(DatumPtr(name));
            advanceToken();
            return node;
        }
    }

    // See if it's a number
    double number = currentToken.wordValue()->numberValue();
    if (currentToken.wordValue()->numberIsValid)
    {
        DatumPtr node(new ASTNode(QObject::tr("number")));
        node.astnodeValue()->genExpression = &Compiler::genLiteral;
        node.astnodeValue()->returnType = RequestReturnDatum;
        node.astnodeValue()->addChild(DatumPtr(number));
        advanceToken();
        return node;
    }

    // If all else fails, it must be a function with the default number of params
    return  parseCommand(false);
}

DatumPtr Parser::parseCommand(bool isVararg)
{
    if (currentToken == nothing)
        return nothing;
    DatumPtr cmdP = currentToken;
    QString cmdString = cmdP.wordValue()->keyValue();

    if (cmdString == ")")
        throw FCError::unexpectedCloseParen();

    int defaultParams = 0;
    int minParams = 0;
    int maxParams = 0;

    DatumPtr node = Config::get().mainProcedures()->astnodeFromCommand(cmdP, minParams, defaultParams, maxParams);

    advanceToken();

    int countOfChildren = 0;
    // isVararg: read all parameters until ')'
    if (isVararg)
    {
        while ((currentToken != nothing) &&
               ((!currentToken.isWord()) || (currentToken.wordValue()->printValue() != ")")))
        {
            DatumPtr child;
            if (minParams < 0)
            {
                child = currentToken;
                advanceToken();
            }
            else
            {
                child = parseExp();
            }
            node.astnodeValue()->addChild(child);
            ++countOfChildren;
        }
    }
    else if (defaultParams < 0)
    { // "Special form": read all parameters until EOL
        while (currentToken != nothing)
        {
            DatumPtr child;
            if (minParams < 0)
            {
                child = currentToken;
                advanceToken();
            }
            else
            {
                child = parseExp();
            }
            node.astnodeValue()->addChild(child);
            ++countOfChildren;
        }
    }
    else
    { // Read in the default number of params
        for (int i = defaultParams; i > 0; --i)
        {
            if (currentToken == nothing)
                throw FCError::notEnoughInputs(cmdP);
            DatumPtr child = parseExp();
            node.astnodeValue()->addChild(child);
            ++countOfChildren;
        }
    }

    if (countOfChildren < minParams)
        throw FCError::notEnoughInputs(node.astnodeValue()->nodeName);
    if ((countOfChildren > maxParams) && (maxParams > -1))
        throw FCError::tooManyInputs(node.astnodeValue()->nodeName);

    return node;
}

void Parser::advanceToken()
{
    if (listIter.elementExists())
    {
        currentToken = listIter.element();
    }
    else
    {
        currentToken = nothing;
    }
}
