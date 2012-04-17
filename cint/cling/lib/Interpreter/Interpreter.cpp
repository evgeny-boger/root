//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
// version: $Id$
// author:  Lukasz Janyst <ljanyst@cern.ch>
//------------------------------------------------------------------------------

#include "cling/Interpreter/Interpreter.h"

#include "DynamicLookup.h"
#include "ExecutionContext.h"
#include "IncrementalParser.h"

#include "cling/Interpreter/CIFactory.h"
#include "cling/Interpreter/CompilationOptions.h"
#include "cling/Interpreter/InterpreterCallbacks.h"
#include "cling/Interpreter/Value.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/DeclarationName.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/Utils.h"
#include "clang/Lex/Preprocessor.h"
#define private public
#include "clang/Parse/Parser.h"
#undef private
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaInternal.h"

#include "llvm/Linker.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_os_ostream.h"

#include <cstdio>
#include <iostream>
#include <sstream>

using namespace clang;

namespace {
static bool tryLinker(const std::string& filename,
                      const cling::InvocationOptions& Opts,
                      llvm::Module* module) {
  assert(module && "Module must exist for linking!");
  llvm::Linker L("cling", module, llvm::Linker::QuietWarnings
                 | llvm::Linker::QuietErrors);
  for (std::vector<llvm::sys::Path>::const_iterator I
         = Opts.LibSearchPath.begin(), E = Opts.LibSearchPath.end(); I != E;
       ++I) {
    L.addPath(*I);
  }
  L.addSystemPaths();
  bool Native = true;
  if (L.LinkInLibrary(filename, Native)) {
    // that didn't work, try bitcode:
    llvm::sys::Path FilePath(filename);
    std::string Magic;
    if (!FilePath.getMagicNumber(Magic, 64)) {
      // filename doesn't exist...
      L.releaseModule();
      return false;
    }
    if (llvm::sys::IdentifyFileType(Magic.c_str(), 64)
        == llvm::sys::Bitcode_FileType) {
      // We are promised a bitcode file, complain if it fails
      L.setFlags(0);
      if (L.LinkInFile(llvm::sys::Path(filename), Native)) {
        L.releaseModule();
        return false;
      }
    } else {
      // Nothing the linker can handle
      L.releaseModule();
      return false;
    }
  } else if (Native) {
    // native shared library, load it!
    llvm::sys::Path SoFile = L.FindLib(filename);
    assert(!SoFile.isEmpty() && "The shared lib exists but can't find it!");
    std::string errMsg;
    bool hasError = llvm::sys::DynamicLibrary
      ::LoadLibraryPermanently(SoFile.str().c_str(), &errMsg);
    if (hasError) {
      llvm::errs() << "Could not load shared library!\n" 
                   << "\n"
                   << errMsg.c_str();
      L.releaseModule();
      return false;
    }
  }
  L.releaseModule();
  return true;
}

static bool canWrapForCall(const std::string& input_line) {
   // Whether input_line can be wrapped into a function.
   // "1" can, "#include <vector>" can't.
   if (input_line.length() > 1 && input_line[0] == '#') return false;
   if (input_line.compare(0, strlen("extern "), "extern ") == 0) return false;
   return true;
}

} // unnamed namespace

namespace cling {

  // "Declared" to the JIT in RuntimeUniverse.h
  namespace runtime {
    namespace internal {
      int local_cxa_atexit(void (*func) (void*), void* arg,
                           void* dso, Interpreter* interp) {
        return interp->CXAAtExit(func, arg, dso);
      }
      struct __trigger__cxa_atexit {
        ~__trigger__cxa_atexit();
      };
      __trigger__cxa_atexit::~__trigger__cxa_atexit() {}
    }
  }

  Interpreter::NamedDeclResult::NamedDeclResult(llvm::StringRef Decl, 
                                                Interpreter* interp, 
                                                const DeclContext* Within)
    : m_Interpreter(interp),
      m_Context(m_Interpreter->getCI()->getASTContext()),
      m_CurDeclContext(Within),
      m_Result(0)
  {
    LookupDecl(Decl);
  }

  Interpreter::NamedDeclResult&
  Interpreter::NamedDeclResult::LookupDecl(llvm::StringRef Decl) {
    DeclarationName Name(&m_Context.Idents.get(Decl));
    DeclContext::lookup_const_result Lookup = m_CurDeclContext->lookup(Name);
    // If more than one found return 0. Cannot handle ambiguities.
    if (Lookup.second - Lookup.first == 1) {
      if (DeclContext* DC = dyn_cast<DeclContext>(*Lookup.first))
        m_CurDeclContext = DC;
      else
        m_CurDeclContext = (*Lookup.first)->getDeclContext();
      
      m_Result = (*Lookup.first);
    }
    else {
      m_Result = 0;
    }

    return *this;
  }

  NamedDecl* Interpreter::NamedDeclResult::getSingleDecl() const {
    // TODO: Check whether it is only one decl if (end-begin == 1 )
    return dyn_cast<NamedDecl>(m_Result);
  }

  const char* DynamicExprInfo::getExpr() {
    int i = 0;
    size_t found;

    while ((found = m_Result.find("@")) && (found != std::string::npos)) { 
      std::stringstream address;
      address << m_Addresses[i];
      m_Result = m_Result.insert(found + 1, address.str());
      m_Result = m_Result.erase(found, 1);
      ++i;
    }

    return m_Result.c_str();
  }

  //
  //  Interpreter
  //
  
  Interpreter::Interpreter(int argc, const char* const *argv, 
                           const char* llvmdir /*= 0*/):
  m_UniqueCounter(0),
  m_PrintAST(false),
  m_ValuePrinterEnabled(false),
  m_LastDump(0)
  {

    std::vector<unsigned> LeftoverArgsIdx;
    m_Opts = InvocationOptions::CreateFromArgs(argc, argv, LeftoverArgsIdx);
    std::vector<const char*> LeftoverArgs;

    for (size_t I = 0, N = LeftoverArgsIdx.size(); I < N; ++I) {
      LeftoverArgs.push_back(argv[LeftoverArgsIdx[I]]);
    }
 
    m_IncrParser.reset(new IncrementalParser(this, LeftoverArgs.size(),
                                             &LeftoverArgs[0],
                                             llvmdir));
    m_ExecutionContext.reset(new ExecutionContext());

    m_ValuePrintStream.reset(new llvm::raw_os_ostream(std::cout));

    // Allow the interpreter to find itself.
    // OBJ first: if it exists it should be more up to date
#ifdef CLING_SRCDIR_INCL
    AddIncludePath(CLING_SRCDIR_INCL);
#endif
#ifdef CLING_INSTDIR_INCL
    AddIncludePath(CLING_INSTDIR_INCL);
#endif

    // Warm them up
    m_IncrParser->Initialize();

    m_ExecutionContext->addSymbol("local_cxa_atexit", 
                  (void*)(intptr_t)&cling::runtime::internal::local_cxa_atexit);

    if (getCI()->getLangOpts().CPlusPlus) {
      // Set up common declarations which are going to be available
      // only at runtime
      // Make sure that the universe won't be included to compile time by using
      // -D __CLING__ as CompilerInstance's arguments
      processLine("#include \"cling/Interpreter/RuntimeUniverse.h\"");

      // Set up the gCling variable
      processLine("#include \"cling/Interpreter/ValuePrinter.h\"\n");
      std::stringstream initializer;
      initializer << "gCling=(cling::Interpreter*)" << (uintptr_t)this << ";";
      processLine(initializer.str());
    }
    else {
      processLine("#include \"cling/Interpreter/CValuePrinter.h\"\n");
    }

    handleFrontendOptions();
  }

  Interpreter::~Interpreter() {
    for (size_t I = 0, N = m_AtExitFuncs.size(); I < N; ++I) {
      const CXAAtExitElement& AEE = m_AtExitFuncs[N - I - 1];
      (*AEE.m_Func)(AEE.m_Arg);
    }
  }

  const char* Interpreter::getVersion() const {
    return "$Id$";
  }

  void Interpreter::handleFrontendOptions() {
    if (m_Opts.ShowVersion) {
      llvm::outs() << getVersion() << '\n';
    }
    if (m_Opts.Help) {
      m_Opts.PrintHelp();
    }
  }
   
  void Interpreter::AddIncludePath(const char *incpath)
  {
    // Add the given path to the list of directories in which the interpreter
    // looks for include files. Only one path item can be specified at a
    // time, i.e. "path1:path2" is not supported.
      
    CompilerInstance* CI = getCI();
    HeaderSearchOptions& headerOpts = CI->getHeaderSearchOpts();
    const bool IsUserSupplied = false;
    const bool IsFramework = false;
    const bool IsSysRootRelative = true;
    headerOpts.AddPath (incpath, frontend::Angled, IsUserSupplied, IsFramework, IsSysRootRelative);
      
    Preprocessor& PP = CI->getPreprocessor();
    ApplyHeaderSearchOptions(PP.getHeaderSearchInfo(), headerOpts,
                                    PP.getLangOpts(),
                                    PP.getTargetInfo().getTriple());      
  }

  // Copied from clang/lib/Frontend/CompilerInvocation.cpp
  void Interpreter::DumpIncludePath() {
    const HeaderSearchOptions Opts(getCI()->getHeaderSearchOpts());
    std::vector<std::string> Res;
    if (Opts.Sysroot != "/") {
      Res.push_back("-isysroot");
      Res.push_back(Opts.Sysroot);
    }

    /// User specified include entries.
    for (unsigned i = 0, e = Opts.UserEntries.size(); i != e; ++i) {
      const HeaderSearchOptions::Entry &E = Opts.UserEntries[i];
      if (E.IsFramework && (E.Group != frontend::Angled || !E.IsUserSupplied))
        llvm::report_fatal_error("Invalid option set!");
      if (E.IsUserSupplied) {
        switch (E.Group) {
        case frontend::After:
          Res.push_back("-idirafter");
          break;
        
        case frontend::Quoted:
          Res.push_back("-iquote");
          break;
        
        case frontend::System:
          Res.push_back("-isystem");
          break;
        
        case frontend::IndexHeaderMap:
          Res.push_back("-index-header-map");
          Res.push_back(E.IsFramework? "-F" : "-I");
          break;
        
        case frontend::CSystem:
          Res.push_back("-c-isystem");
          break;

        case frontend::CXXSystem:
          Res.push_back("-cxx-isystem");
          break;

        case frontend::ObjCSystem:
          Res.push_back("-objc-isystem");
          break;

        case frontend::ObjCXXSystem:
          Res.push_back("-objcxx-isystem");
          break;
        
        case frontend::Angled:
          Res.push_back(E.IsFramework ? "-F" : "-I");
          break;
        }
      } else {
        if (E.Group != frontend::Angled && E.Group != frontend::System)
          llvm::report_fatal_error("Invalid option set!");
        Res.push_back(E.Group == frontend::Angled ? "-iwithprefixbefore" :
                      "-iwithprefix");
      }
      Res.push_back(E.Path);
    }

    if (!Opts.ResourceDir.empty()) {
      Res.push_back("-resource-dir");
      Res.push_back(Opts.ResourceDir);
    }
    if (!Opts.ModuleCachePath.empty()) {
      Res.push_back("-fmodule-cache-path");
      Res.push_back(Opts.ModuleCachePath);
    }
    if (!Opts.UseStandardSystemIncludes)
      Res.push_back("-nostdinc");
    if (!Opts.UseStandardCXXIncludes)
      Res.push_back("-nostdinc++");
    if (Opts.UseLibcxx)
      Res.push_back("-stdlib=libc++");
    if (Opts.Verbose)
      Res.push_back("-v");

    // print'em all
    for (unsigned i = 0; i < Res.size(); ++i) {
      llvm::errs() << Res[i] <<"\n";
    }
  }

  CompilerInstance* Interpreter::getCI() const {
    return m_IncrParser->getCI();
  }

  Parser* Interpreter::getParser() const {
    return m_IncrParser->getParser();
  }

  ///\brief Maybe transform the input line to implement cint command line 
  /// semantics (declarations are global) and compile to produce a module.
  ///
  Interpreter::CompilationResult
  Interpreter::processLine(const std::string& input_line, 
                           bool rawInput /*=false*/,
                           Value* V, /*=0*/
                           const Decl** D /*=0*/) {
    DiagnosticsEngine& Diag = getCI()->getDiagnostics();
    // Disable warnings which doesn't make sense when using the prompt
    // This gets reset with the clang::Diagnostics().Reset()
    Diag.setDiagnosticMapping(clang::diag::warn_unused_expr,
                              clang::diag::MAP_IGNORE, SourceLocation());
    Diag.setDiagnosticMapping(clang::diag::warn_unused_call,
                              clang::diag::MAP_IGNORE, SourceLocation());

    CompilationOptions CO;
    CO.DeclarationExtraction = 1;
    CO.ValuePrinting = 2;
    CO.DynamicScoping = isDynamicLookupEnabled();
    CO.Debug = isPrintingAST();

    if (rawInput)
      return Declare(input_line, CO, D);

    if (!canWrapForCall(input_line))
      return declare(input_line, D);


    if (V) {
      return Evaluate(input_line, CO, V);
    }
    
    std::string functName;
    std::string wrapped = input_line;
    WrapInput(wrapped, functName);

    if (m_IncrParser->Compile(wrapped, CO) == IncrementalParser::kFailed)
        return Interpreter::kFailure;

    if (D)
      *D = m_IncrParser->getLastTransaction().getFirstDecl();

    //
    //  Run it using the JIT.
    //
    // TODO: Handle the case when RunFunction wasn't able to run the function
    bool RunRes = RunFunction(functName);

    if (RunRes)
      return Interpreter::kSuccess;

    return Interpreter::kFailure;
  }

  Interpreter::CompilationResult
  Interpreter::declare(const std::string& input, const Decl** D /* = 0 */) {
    CompilationOptions CO;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;

    return Declare(input, CO, D);
  }

  Interpreter::CompilationResult
  Interpreter::evaluate(const std::string& input, Value* V /* = 0 */) {
    // Here we might want to enforce further restrictions like: Only one
    // ExprStmt can be evaluated and etc. Such enforcement cannot happen in the
    // worker, because it is used from various places, where there is no such
    // rule
    CompilationOptions CO;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;

    return Evaluate(input, CO, V);
  }

  Interpreter::CompilationResult
  Interpreter::echo(const std::string& input, Value* V /* = 0 */) {
    CompilationOptions CO;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 2;

    return Evaluate(input, CO, V);
  }

  void Interpreter::WrapInput(std::string& input, std::string& fname) {
    fname = createUniqueWrapper();
    input.insert(0, "void " + fname + "() {\n ");
    input.append("\n;\n}");
  }

  llvm::StringRef Interpreter::createUniqueWrapper() {
    const size_t size = sizeof("__cling_Un1Qu3") + sizeof(m_UniqueCounter);
    llvm::SmallString<size> out("__cling_Un1Qu3");
    llvm::raw_svector_ostream(out) << m_UniqueCounter++;

    return (getCI()->getASTContext().Idents.getOwn(out)).getName();
  }

  bool Interpreter::RunFunction(llvm::StringRef fname, llvm::GenericValue* res) {
    if (getCI()->getDiagnostics().hasErrorOccurred())
      return false;

    if (m_IncrParser->isSyntaxOnly()) {
      return true;
    }

    std::string mangledNameIfNeeded;
    FunctionDecl* FD = cast_or_null<FunctionDecl>(LookupDecl(fname).
                                                  getSingleDecl()
                                                  );
    if (FD) {
      if (!FD->isExternC()) {
        llvm::raw_string_ostream RawStr(mangledNameIfNeeded);
        llvm::OwningPtr<MangleContext> 
          Mangle(getCI()->getASTContext().createMangleContext());
        Mangle->mangleName(FD, RawStr);
        RawStr.flush();
        fname = mangledNameIfNeeded;
      }
      m_ExecutionContext->executeFunction(fname, res);
      return true;
    }

    return false;
  }

  void Interpreter::createUniqueName(std::string& out) {
    out = "Un1Qu3";
    llvm::raw_string_ostream(out) << m_UniqueCounter++;
  }

  Interpreter::CompilationResult
  Interpreter::Declare(const std::string& input, const CompilationOptions& CO,
                       const clang::Decl** D /* = 0 */) {

    if (m_IncrParser->Compile(input, CO) != IncrementalParser::kFailed) {
      if (D)
        *D = m_IncrParser->getLastTransaction().getFirstDecl();
      return Interpreter::kSuccess;
    }

    return Interpreter::kFailure;
  }

  Interpreter::CompilationResult
  Interpreter::Evaluate(const std::string& input, const CompilationOptions& CO,
                        Value* V /* = 0 */) {

    Sema& TheSema = getCI()->getSema();

    // Wrap the expression
    std::string WrapperName;
    std::string Wrapper = input;
    WrapInput(Wrapper, WrapperName);

    llvm::SmallVector<clang::DeclGroupRef, 4> DGRs;
    m_IncrParser->Parse(Wrapper, DGRs);
    assert(DGRs.size() && "No decls created by Parse!");

    // Find the wrapper function declaration.
    //
    // Note: The parse may have created a whole set of decls if a template
    //       instantiation happened.  Our wrapper function should be the
    //       last decl in the set.
    //
    FunctionDecl* TopLevelFD 
      = dyn_cast<FunctionDecl>(DGRs.back().getSingleDecl());
    assert(TopLevelFD && "No Decls Parsed?");
    DeclContext* CurContext = TheSema.CurContext;
    TheSema.CurContext = TopLevelFD;
    ASTContext& Context(getCI()->getASTContext());
    QualType RetTy;
    // We have to be able to mark the expression for printout. There are three
    // scenarios:
    // 0: Expression printing disabled - don't do anything just disable the 
    //    consumer
    //    is our marker, even if there wasn't missing ';'.
    // 1: Expression printing enabled - make sure we don't have NullStmt, which
    //    is used as a marker to suppress the print out.
    // 2: Expression printing auto - do nothing - rely on the omitted ';' to 
    //    not produce the suppress marker.
    if (CompoundStmt* CS = dyn_cast<CompoundStmt>(TopLevelFD->getBody())) {
      // Collect all Stmts, contained in the CompoundStmt
      llvm::SmallVector<Stmt *, 4> Stmts;
      for (CompoundStmt::body_iterator iStmt = CS->body_begin(), 
             eStmt = CS->body_end(); iStmt != eStmt; ++iStmt)
        Stmts.push_back(*iStmt);

      size_t indexOfLastExpr = Stmts.size();
      while(indexOfLastExpr--) {
        // find the trailing expression statement (skip e.g. null statements)
        if (Expr* E = dyn_cast_or_null<Expr>(Stmts[indexOfLastExpr])) {
          RetTy = E->getType();
          if (!RetTy->isVoidType()) {
            // Change the void function's return type
            FunctionProtoType::ExtProtoInfo EPI;
            QualType FuncTy = Context.getFunctionType(RetTy,/* ArgArray = */0,
                                                      /* NumArgs = */0, EPI);
            TopLevelFD->setType(FuncTy);
            // Strip the parenthesis if any
            //if (ParenExpr* PE = dyn_cast<ParenExpr>(E))
            //  E = PE->getSubExpr();

            // Change it with return stmt
            Stmts[indexOfLastExpr] 
              = TheSema.ActOnReturnStmt(SourceLocation(), E).take();
          }
          // even if void: we found an expression
          break;
        }
      }

      // case 1:
      if (CO.ValuePrinting == CompilationOptions::Enabled) 
        if (indexOfLastExpr < Stmts.size() - 1 && 
            isa<NullStmt>(Stmts[indexOfLastExpr + 1]))
          Stmts.erase(Stmts.begin() + indexOfLastExpr);
          // Stmts.insert(Stmts.begin() + indexOfLastExpr + 1, 
          //              TheSema.ActOnNullStmt(SourceLocation()).take());

      // Update the CompoundStmt body
      CS->setStmts(TheSema.getASTContext(), Stmts.data(), Stmts.size());

    }

    TheSema.CurContext = CurContext;

    // FIXME: Finish the transaction in better way
    m_IncrParser->Compile("", CO);

    // get the result
    llvm::GenericValue val;
    if (RunFunction(WrapperName, &val)) {
      if (V)
        *V = Value(val, RetTy.getTypePtrOrNull());

      return Interpreter::kSuccess;
    }

    return Interpreter::kFailure;
  }

  bool
  Interpreter::loadFile(const std::string& filename,
                        bool allowSharedLib /*=true*/)
  {
    if (allowSharedLib) {
      llvm::Module* module = m_IncrParser->GetCodeGenerator()->GetModule();
      if (module) {
        if (tryLinker(filename, getOptions(), module))
          return true;
        if (filename.compare(0, 3, "lib") == 0) {
          // starts with "lib", try without (the llvm::Linker forces
          // a "lib" in front, which makes it liblib...
          if (tryLinker(filename.substr(3, std::string::npos),
                        getOptions(), module))
            return true;
        }
      }
    }
    
    std::string code;
    code += "#include \"" + filename + "\"\n";
    return declare(code);
  }
  
  Decl*
  Interpreter::lookupClass(const std::string& className)
  {
    //
    //  Our return value.
    //
    Decl* TheDecl = 0;
    //
    //  Some utilities.
    //
    CompilerInstance* CI = getCI();
    Parser* P = getParser();
    Preprocessor& PP = CI->getPreprocessor();
    //
    //  Tell the diagnostic engine to ignore all diagnostics.
    //
    bool OldSuppressAllDiagnostics =
      PP.getDiagnostics().getSuppressAllDiagnostics();
    PP.getDiagnostics().setSuppressAllDiagnostics(true);
    //
    //  Tell the diagnostic consumer we are switching files.
    //
    DiagnosticConsumer& DClient = CI->getDiagnosticClient();
    DClient.BeginSourceFile(CI->getLangOpts(), &PP);
    //
    //  Convert the class name to a nested name specifier for parsing.
    //
    std::string classNameAsNNS = className + "::\n";
    //
    //  Create a fake file to parse the class name.
    //
    llvm::MemoryBuffer* SB = llvm::MemoryBuffer::getMemBufferCopy(
      classNameAsNNS, "lookup.class.by.name.file");
    FileID FID = CI->getSourceManager().createFileIDForMemBuffer(SB);
    //
    //  Turn on ignoring of the main file eof token.
    //
    //  Note: We need this because token readahead in the following
    //        routine calls ends up parsing it multiple times.
    //
    bool ResetIncrementalProcessing = false;
    if (!PP.isIncrementalProcessingEnabled()) {
      ResetIncrementalProcessing = true;
      PP.enableIncrementalProcessing();
    }
    //
    //  Switch to the new file the way #include does.
    //
    //  Note: To switch back to the main file we must consume an eof token.
    //
    PP.EnterSourceFile(FID, 0, SourceLocation());
    PP.Lex(const_cast<Token&>(P->getCurToken()));
    //
    //  Parse the class name as a nested-name-specifier.
    //
    CXXScopeSpec SS;
    bool MayBePseudoDestructor = false;
    if (P->ParseOptionalCXXScopeSpecifier(
        SS, /*ObjectType*/ParsedType(), /*EnteringContext*/false,
        &MayBePseudoDestructor, /*IsTypename*/false)) {
      // error path
      goto lookupClassDone;
    }
    //
    //  Check for a valid result.
    //
    if (!SS.isValid()) {
      // error path
      goto lookupClassDone;
    }
    //
    //  Be careful, not all nested name specifiers refer to classes
    //  and namespaces, and those are the only things we want.
    //
    {
      NestedNameSpecifier* NNS = SS.getScopeRep();
      NestedNameSpecifier::SpecifierKind Kind = NNS->getKind();
      switch (Kind) {
        case NestedNameSpecifier::Identifier:
        case NestedNameSpecifier::Global:
          // We do not want these.
          break;
        case NestedNameSpecifier::Namespace: {
            NamespaceDecl* NSD = NNS->getAsNamespace();
            NSD = NSD->getCanonicalDecl();
            TheDecl = NSD;
          }
          break;
        case NestedNameSpecifier::NamespaceAlias: {
            // Note: In the future, should we return the alias instead? 
            NamespaceAliasDecl* NSAD = NNS->getAsNamespaceAlias();
            NamespaceDecl* NSD = NSAD->getNamespace();
            NSD = NSD->getCanonicalDecl();
            TheDecl = NSD;
          }
          break;
        case NestedNameSpecifier::TypeSpec:
        case NestedNameSpecifier::TypeSpecWithTemplate: {
            // Note: Do we need to check for a dependent type here?
            const Type* Ty = NNS->getAsType();
            const TagType* TagTy = Ty->getAs<TagType>();
            if (TagTy) {
              // It is a class, struct, union, or enum.
              TagDecl* TD = TagTy->getDecl();
              if (TD) {
                // Make sure it is not just forward declared, and instantiate
                // any templates.
                if (!CI->getSema().RequireCompleteDeclContext(SS, TD)) {
                  // Success, type is complete, instantiations have been done.
                  TagDecl* Def = TD->getDefinition();
                  if (Def) {
                    TheDecl = Def;
                  }
                }
              }
            }
          }
          break;
      }
    }
  lookupClassDone:
    //
    // Advance the parser to the end of the file, and pop the include stack.
    //
    // Note: Consuming the EOF token will pop the include stack.
    //
    P->SkipUntil(tok::eof, /*StopAtSemi*/false, /*DontConsume*/false,
      /*StopAtCodeCompletion*/false);
    if (ResetIncrementalProcessing) {
      PP.enableIncrementalProcessing(false);
    }
    DClient.EndSourceFile();
    CI->getDiagnostics().Reset();
    PP.getDiagnostics().setSuppressAllDiagnostics(OldSuppressAllDiagnostics);
    return TheDecl;
  }

  Interpreter::NamedDeclResult Interpreter::LookupDecl(llvm::StringRef Decl, 
                                                       const DeclContext* Within) {
    if (!Within)
      Within = getCI()->getASTContext().getTranslationUnitDecl();
    return Interpreter::NamedDeclResult(Decl, this, Within);
  }

  void Interpreter::installLazyFunctionCreator(void* (*fp)(const std::string&)) {
    m_ExecutionContext->installLazyFunctionCreator(fp);
  }
  
  Value Interpreter::Evaluate(const char* expr, DeclContext* DC,
                              bool ValuePrinterReq) {
    Sema& TheSema = getCI()->getSema();
    if (!DC)
      DC = TheSema.getASTContext().getTranslationUnitDecl();
    
    // Set up the declaration context
    DeclContext* CurContext;

    CurContext = TheSema.CurContext;
    TheSema.CurContext = DC;

    Value Result;
    if (TheSema.ExternalSource) {
      DynamicIDHandler* DIDH = 
        static_cast<DynamicIDHandler*>(TheSema.ExternalSource);
      DIDH->Callbacks->setEnabled();
      (ValuePrinterReq) ? echo(expr, &Result) : evaluate(expr, &Result);
      DIDH->Callbacks->setEnabled(false);
    }
    else 
      (ValuePrinterReq) ? echo(expr, &Result) : evaluate(expr, &Result);

    return Result;
  }

  void Interpreter::setCallbacks(InterpreterCallbacks* C) {
    Sema& S = getCI()->getSema();
    assert(S.ExternalSource && "No ExternalSource set!");
    static_cast<DynamicIDHandler*>(S.ExternalSource)->Callbacks = C;
  }

  void Interpreter::enableDynamicLookup(bool value /*=true*/) {
    m_IncrParser->enableDynamicLookup(value);
  }

  bool Interpreter::isDynamicLookupEnabled() {
    return m_IncrParser->isDynamicLookupEnabled();
  }

  void Interpreter::enablePrintAST(bool print /*=true*/) {
    m_IncrParser->enablePrintAST(print);
    m_PrintAST = !m_PrintAST;
  }

  void Interpreter::runStaticInitializersOnce() const {
    // Forward to ExecutionContext; should not be called by
    // anyone except for IncrementalParser.
    llvm::Module* module = m_IncrParser->GetCodeGenerator()->GetModule();
    m_ExecutionContext->runStaticInitializersOnce(module);
  }

  int Interpreter::CXAAtExit(void (*func) (void*), void* arg, void* dso) {
     // Register a CXAAtExit function
     clang::Decl* LastTLD = m_IncrParser->getLastTopLevelDecl();
     m_AtExitFuncs.push_back(CXAAtExitElement(func, arg, dso, LastTLD));
     return 0; // happiness
  }

  bool Interpreter::addSymbol(const char* symbolName,  void* symbolAddress){
    // Forward to ExecutionContext;
    if (!symbolName || !symbolAddress )
      return false;

    return m_ExecutionContext->addSymbol(symbolName,  symbolAddress);
  }
  
} // namespace cling

