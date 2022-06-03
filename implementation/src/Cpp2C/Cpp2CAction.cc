#include "Cpp2C/Cpp2CAction.hh"
#include "Transformer/TransformerConsumer.hh"
#include "AnnotationRemover/AnnotationRemoverConsumer.hh"

namespace Cpp2C
{
    using namespace std;
    using namespace clang;

    string USAGE_STRING = "USAGE: cpp2c <(transform|tr [(--overwrite-files|-ow)|(--verbose|-v)|(--standard-header-macros|-shm)*])|(remove_annotations|ra)> FILE_NAME";

    unique_ptr<ASTConsumer>
    Cpp2CAction::CreateASTConsumer(
        CompilerInstance &CI,
        StringRef file)
    {
        // Check which command the user passed, create the appropriate AST
        // consumer for it, and return it
        if (Command == HELP)
        {
            // This shouldn't happen since the ParseArgs method should
            // return false before we reach this point, but if it does,
            // Clang will crash
            llvm::errs() << USAGE_STRING << "\n";
            exit(1);
        }
        else if (Command == TRANSFORM)
        {
            auto Tr = make_unique<Transformer::TransformerConsumer>(&CI, TSettings);
            return Tr;
        }
        else if (Command == REMOVE_ANNOTATIONS)
        {
            auto AR = make_unique<AnnotationRemover::AnnotationRemoverConsumer>(ARSettings);
            return AR;
        }
        llvm::errs() << "No command passed\n";
        exit(1);
    }

    bool Cpp2CAction::ParseArgs(
        const CompilerInstance &CI,
        const vector<string> &args)
    {
        // README
        // First argument is the command
        // Rest are optional arguments for that command

        // Check that the user passed a required command
        if (args.size() == 0)
        {
            llvm::errs() << USAGE_STRING << "\n";
            return false;
        }

        // Parse the command, advance the args iterator, and parse the
        // rest of the arguments for this command
        auto &command = args.front();
        auto optionalArgs = args.begin();
        optionalArgs++;

        // Transform
        if (command == "tr" || command == "transform")
        {
            Command = TRANSFORM;
            for (auto it = optionalArgs; it != args.end(); ++it)
            {
                std::string arg = *it;
                if (arg == "-ow" || arg == "--overwrite-files")
                {
                    TSettings.OverwriteFiles = true;
                }
                else if (arg == "-v" || arg == "--verbose")
                {
                    TSettings.Verbose = true;
                }
                else if (arg == "-shm" || arg == "--standard-header-macros")
                {
                    TSettings.OnlyCollectNotDefinedInStdHeaders = false;
                }
                else
                {
                    llvm::errs() << "Unknown transformer argument: " << arg << '\n';
                    exit(1);
                }
            }
        }

        // Remove annotations
        else if (command == "ra" || command == "remove_annotations")
        {
            Command = REMOVE_ANNOTATIONS;
            for (auto it = optionalArgs; it != args.end(); ++it)
            {
                std::string arg = *it;
                if (arg == "-ow" || arg == "--overwrite-files")
                {
                    TSettings.OverwriteFiles = true;
                }
                else
                {
                    llvm::errs() << "Unknown annotation remover argument: " << arg << '\n';
                    exit(1);
                }
            }
        }

        // No valid command passed
        else
        {
            llvm::errs() << USAGE_STRING << "\n";
            return false;
        }

        return true;
    }

    // Necessary for ANYTHING to print to stderr
    PluginASTAction::ActionType Cpp2CAction::getActionType()
    {
        return ActionType::AddBeforeMainAction;
    }

} // namespace Cpp2C
