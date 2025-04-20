//===-- qlogo/workspacemanagement.cpp - Workspace Management implementations -------*- C++ -*-===//
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
/// This file contains the implementation of the workspacemanagements methods
/// of the Compiler class.
///
//===----------------------------------------------------------------------===//

#include "compiler_private.h"
#include "flowcontrol.h"
#include "astnode.h"
#include "compiler.h"
#include "workspace/callframe.h"
#include "sharedconstants.h"
#include "kernel.h"

/// @brief input a procedure using the system read stream.
/// @param eAddr a pointer to the Evaluator object
/// @param nodeAddr a pointer to the node calling this procedure.
/// @return ASTNode on success, else Err.
EXPORTC addr_t inputProcedure(addr_t eAddr, addr_t nodeAddr)
{
    Evaluator *e = reinterpret_cast<Evaluator *>(eAddr);
    ASTNode *node = reinterpret_cast<ASTNode*>(nodeAddr);
    DatumPtr currentProc = Config::get().mainKernel()->callStack.localFrame()->sourceNode;
    if (!currentProc.isNothing())
    {
        FCError *err = FCError::toInProc(node);
        e->watch(err);
        return reinterpret_cast<addr_t>(err);
    }

    return reinterpret_cast<addr_t>(Config::get().mainKernel()->inputProcedure(node));
}

using namespace llvm;
using namespace llvm::orc;

// PROCEDURE DEFINITION

/***DOC TO
TO procname :input1 :input2 ...                         (special form)

 command.  Prepares Logo to accept a procedure definition.  The
 procedure will be named "procname" and there must not already
 be a procedure by that name.  The inputs will be called "input1"
 etc.  Any number of inputs are allowed, including none.  Names
 of procedures and inputs are case-insensitive.

 Unlike every other Logo procedure, TO takes as its inputs the
 actual words typed in the instruction line, as if they were
 all quoted, rather than the results of evaluating expressions
 to provide the inputs.  (That's what "special form" means.)

 This version of Logo allows variable numbers of inputs to a
 procedure.  After the procedure name come four kinds of
 things, *in this order*:

     1.   0 or more REQUIRED inputs    :FOO :FROBOZZ
     2.   0 or more OPTIONAL inputs    [:BAZ 87] [:THINGO 5+9]
     3.   0 or 1 REST input            [:GARPLY]
     4.   0 or 1 DEFAULT number        5

 Every procedure has a MINIMUM, DEFAULT, and MAXIMUM
 number of inputs.  (The latter can be infinite.)

 The MINIMUM number of inputs is the number of required inputs,
 which must come first.  A required input is indicated by the

         :inputname

 notation.

 After all the required inputs can be zero or more optional inputs,
 each of which is represented by the following notation:

         [:inputname default.value.expression]

 When the procedure is invoked, if actual inputs are not supplied
 for these optional inputs, the default value expressions are
 evaluated to set values for the corresponding input names.  The
 inputs are processed from left to right, so a default value
 expression can be based on earlier inputs.  Example:

         to proc :inlist [:startvalue first :inlist]

 If the procedure is invoked by saying

         proc [a b c]

 then the variable INLIST will have the value [A B C] and the
 variable STARTVALUE will have the value A.  If the procedure
 is invoked by saying

         (proc [a b c] "x)

 then INLIST will have the value [A B C] and STARTVALUE will
 have the value X.

 After all the required and optional input can come a single "rest"
 input, represented by the following notation:

         [:inputname]

 This is a rest input rather than an optional input because there
 is no default value expression.  There can be at most one rest
 input.  When the procedure is invoked, the value of this inputname
 will be a list containing all of the actual inputs provided that
 were not used for required or optional inputs.  Example:

         to proc :in1 [:in2 "foo] [:in3 "baz] [:in4]

 If this procedure is invoked by saying

         proc "x

 then IN1 has the value X, IN2 has the value FOO, IN3 has the value
 BAZ, and IN4 has the value [] (the empty list).  If it's invoked
 by saying

         (proc "a "b "c "d "e)

 then IN1 has the value A, IN2 has the value B, IN3 has the value C,
 and IN4 has the value [D E].

 The MAXIMUM number of inputs for a procedure is infinite if a
 rest input is given; otherwise, it is the number of required
 inputs plus the number of optional inputs.

 The DEFAULT number of inputs for a procedure, which is the number
 of inputs that it will accept if its invocation is not enclosed
 in parentheses, is ordinarily equal to the minimum number.  If
 you want a different default number you can indicate that by
 putting the desired default number as the last thing on the
 TO line.  example:

         to proc :in1 [:in2 "foo] [:in3] 3

 This procedure has a minimum of one input, a default of three
 inputs, and an infinite maximum.

 Logo responds to the TO command by entering procedure definition
 mode.  The prompt character changes from "?" to ">" and whatever
 instructions you type become part of the definition until you
 type a line containing only the word END.

COD***/
// CMD TO -1 -1 -1 n
// CMD .MACRO -1 -1 -1 n
Value *Compiler::genInputProcedure(DatumPtr node, RequestReturnType returnType)
{
    Q_ASSERT(returnType && RequestReturnNothing);
    // We take the parameters as literals; we don't generate children.
    // Pass the node directly to the input function.
    return generateCallExtern(TyAddr, "inputProcedure", {PaAddr(evaluator), PaAddr(CoAddr(node.datumValue()))});
}


/***DOC MAKE
MAKE varname value

command.  Assigns the value "value" to the variable named "varname",
which must be a word.  Variable names are case-insensitive.  If a
variable with the same name already exists, the value of that
variable is changed.  If not, a new global variable is created.

COD***/
// CMD MAKE 2 2 2 n
Value *Compiler::genMake(DatumPtr node, RequestReturnType returnType)
{
    Q_ASSERT(returnType && RequestReturnNothing);

    std::vector<RequestReturnType> returnTypeAry = {RequestReturnDatum, RequestReturnDatum};
    std::vector<Value *> children = generateChildren(node.astnodeValue(), returnTypeAry);
    Value *varname = generateFromDatum(Datum::typeWord, node.astnodeValue(), children[0]);
    Value *value = children[1];

    generateCallExtern(TyVoid, "setDatumForWord", {PaAddr(value), PaAddr(varname)});
    return generateVoidRetval(node);
}
