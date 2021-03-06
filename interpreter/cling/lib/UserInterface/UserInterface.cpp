//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
// author:  Axel Naumann <axel@cern.ch>
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.
//------------------------------------------------------------------------------

#include "cling/UserInterface/UserInterface.h"

#include "cling/Interpreter/Exception.h"
#include "cling/MetaProcessor/MetaProcessor.h"
#include "textinput/Callbacks.h"
#include "textinput/TextInput.h"
#include "textinput/StreamReader.h"
#include "textinput/TerminalDisplay.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Path.h"
#include "llvm/Config/config.h"

#include "clang/Basic/LangOptions.h"
#include "clang/Frontend/CompilerInstance.h"

// Fragment copied from LLVM's raw_ostream.cpp
#if defined(HAVE_UNISTD_H)
# include <unistd.h>
#endif

#if defined(_MSC_VER)
#ifndef STDIN_FILENO
# define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
# define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
# define STDERR_FILENO 2
#endif
#endif

#include <memory>

namespace {
  ///\brief Class that specialises the textinput TabCompletion to allow Cling
  /// to code complete through its own textinput mechanism which is part of the
  /// UserInterface.
  ///
  class UITabCompletion : public textinput::TabCompletion {
    const cling::Interpreter& m_ParentInterpreter;
  
  public:
    UITabCompletion(const cling::Interpreter& Parent) :
                    m_ParentInterpreter(Parent) {}
    ~UITabCompletion() {}

    bool Complete(textinput::Text& Line /*in+out*/,
                  size_t& Cursor /*in+out*/,
                  textinput::EditorRange& R /*out*/,
                  std::vector<std::string>& Completions /*out*/) override {
      m_ParentInterpreter.codeComplete(Line.GetText(), Cursor, Completions);
      return true;
    }
  };
}

namespace cling {

  UserInterface::UserInterface(Interpreter& interp) {
    // We need stream that doesn't close its file descriptor, thus we are not
    // using llvm::outs. Keeping file descriptor open we will be able to use
    // the results in pipes (Savannah #99234).
    static llvm::raw_fd_ostream m_MPOuts (STDOUT_FILENO, /*ShouldClose*/false);
    m_MetaProcessor.reset(new MetaProcessor(interp, m_MPOuts));
    llvm::install_fatal_error_handler(&CompilationException::throwingHandler);
  }

  UserInterface::~UserInterface() {}

  void UserInterface::runInteractively(bool nologo /* = false */) {
    if (!nologo) {
      PrintLogo();
    }

    llvm::SmallString<512> histfilePath;
    if (!getenv("CLING_NOHISTORY")) {
      // History file is $HOME/.cling_history
      if (llvm::sys::path::home_directory(histfilePath))
        llvm::sys::path::append(histfilePath, ".cling_history");
    }

    using namespace textinput;
    std::unique_ptr<StreamReader> R(StreamReader::Create());
    std::unique_ptr<TerminalDisplay> D(TerminalDisplay::Create());
    TextInput TI(*R, *D, histfilePath.empty() ? 0 : histfilePath.c_str());

    // Inform text input about the code complete consumer
    // TextInput owns the TabCompletion.
    UITabCompletion* Completion =
                      new UITabCompletion(m_MetaProcessor->getInterpreter());
    TI.SetCompletion(Completion);

    std::string Line;
    std::string Prompt("[cling]$ ");

    while (true) {
      try {
        m_MetaProcessor->getOuts().flush();
        {
          MetaProcessor::MaybeRedirectOutputRAII RAII(*m_MetaProcessor);
          TI.SetPrompt(Prompt.c_str());
          if (TI.ReadInput() == TextInput::kRREOF)
            break;
          TI.TakeInput(Line);
        }

        cling::Interpreter::CompilationResult compRes;
        const int indent = m_MetaProcessor->process(Line.c_str(), compRes);

        // Quit requested?
        if (indent < 0)
          break;

        Prompt.replace(7, std::string::npos,
           m_MetaProcessor->getInterpreter().isRawInputEnabled() ? "! " : "$ ");

        // Continuation requested?
        if (indent > 0) {
          Prompt.append(1, '?');
          Prompt.append(indent * 3, ' ');
        }
      }
      catch(InvalidDerefException& e) {
        e.diagnose();
      }
      catch(InterpreterException& e) {
        llvm::errs() << ">>> Caught an interpreter exception!\n"
                     << ">>> " << e.what() << '\n';
      }
      catch(std::exception& e) {
        llvm::errs() << ">>> Caught a std::exception!\n"
                     << ">>> " << e.what() << '\n';
      }
      catch(...) {
        llvm::errs() << "Exception occurred. Recovering...\n";
      }
    }
    m_MetaProcessor->getOuts().flush();
  }

  void UserInterface::PrintLogo() {
    llvm::raw_ostream& outs = m_MetaProcessor->getOuts();
    const clang::LangOptions& LangOpts
      = m_MetaProcessor->getInterpreter().getCI()->getLangOpts();
    if (LangOpts.CPlusPlus) {
      outs << "\n"
        "****************** CLING ******************\n"
        "* Type C++ code and press enter to run it *\n"
        "*             Type .q to exit             *\n"
        "*******************************************\n";
    } else {
      outs << "\n"
        "***************** CLING *****************\n"
        "* Type C code and press enter to run it *\n"
        "*            Type .q to exit            *\n"
        "*****************************************\n";
    }
  }
} // end namespace cling
