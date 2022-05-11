#pragma once

#include "Transformer/TransformerSettings.hh"
#include "CppSig/MacroExpansionNode.hh"
#include "CppSig/MacroForest.hh"

#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/CompilerInstance.h"

#include <set>
#include <string>
#include <map>

namespace Transformer
{

    void emitUntransformedMessage(
        clang::ASTContext &Ctx,
        CppSig::MacroExpansionNode *Expansion,
        std::string Category,
        std::string Reason);

    // AST consumer which calls the visitor class to perform the transformation
    class TransformerConsumer : public clang::ASTConsumer
    {
    private:
        clang::CompilerInstance *CI;
        clang::Preprocessor &PP;
        CppSig::MacroForest::Roots ExpansionRoots;
        std::set<std::string> MacroNames;
        std::set<std::string> MultiplyDefinedMacros;
        std::map<std::string, std::set<std::string>> MacroDefinitionToTransformedDefinitionPrototypes;

        // To give it access to members
        friend class TransformerAction;

        TransformerSettings Cpp2CSettings;

    public:
        explicit TransformerConsumer(clang::CompilerInstance *CI, TransformerSettings Cpp2CSettings);

        virtual void HandleTranslationUnit(clang::ASTContext &Ctx);
    };
} // namespace Transformer