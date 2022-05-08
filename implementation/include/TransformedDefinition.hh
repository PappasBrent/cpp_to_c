#pragma once

#include "MacroExpansionNode.hh"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Type.h"

#include <string>
#include <vector>

class TransformedDefinition
{
    friend class Cpp2CConsumer;

    // The original expansion that we are transforming
    MacroExpansionNode *Expansion;
    // The name of the original macro that this transformation came from
    std::string OriginalMacroName;
    // Whether this transformation is to a variable or a function
    bool IsVar;
    // The type of the variable we transform to, or the return type of the
    // function if we are transforming to a function
    clang::QualType VarOrReturnType;
    // A vector of the types of the transformed function's arguments
    std::vector<clang::QualType> ArgTypes;
    // The body of the transformed definition
    std::string InitializerOrDefinition;
    // The name used when emitting this definition
    std::string EmittedName;

public:
    TransformedDefinition(
        clang::ASTContext &Ctx,
        MacroExpansionNode *Expansion,
        bool isVar);

    // Gets the signature for this transformed expansion if it's a function;
    // otherwise gets the declaration
    std::string getExpansionSignatureOrDeclaration(
        clang::ASTContext &Ctx,
        bool CanBeAnonymous);

    // Returns true if the transformed function signature contains a
    // user-defined type
    bool hasNonBuiltinTypes();

    // Returns true if the transformed function signature contains a
    // an array type
    bool hasArrayTypes();

    // Returns true if the transformed function signature contains a function
    // type or function pointer type
    bool hasFunctionTypes();
};