/*============================================================================
  CMake - Cross Platform Makefile Generator
  Copyright 2000-2009 Kitware, Inc., Insight Software Consortium

  Distributed under the OSI-approved BSD License (the "License");
  see accompanying file Copyright.txt for details.

  This software is distributed WITHOUT ANY WARRANTY; without even the
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the License for more information.
============================================================================*/
#include "cmTarget.h"
#include "cmake.h"
#include "cmMakefile.h"
#include "cmSourceFile.h"
#include "cmLocalGenerator.h"
#include "cmGlobalGenerator.h"
#include "cmListFileCache.h"
#include "cmGeneratorExpression.h"
#include "cmGeneratorExpressionDAGChecker.h"
#include <cmsys/RegularExpression.hxx>
#include <map>
#include <set>
#include <stdlib.h> // required for atof
#include <assert.h>
#include <errno.h>

const char* cmTarget::GetTargetTypeName(TargetType targetType)
{
  switch( targetType )
    {
      case cmTarget::STATIC_LIBRARY:
        return "STATIC_LIBRARY";
      case cmTarget::MODULE_LIBRARY:
        return "MODULE_LIBRARY";
      case cmTarget::SHARED_LIBRARY:
        return "SHARED_LIBRARY";
      case cmTarget::OBJECT_LIBRARY:
        return "OBJECT_LIBRARY";
      case cmTarget::EXECUTABLE:
        return "EXECUTABLE";
      case cmTarget::UTILITY:
        return "UTILITY";
      case cmTarget::GLOBAL_TARGET:
        return "GLOBAL_TARGET";
      case cmTarget::INTERFACE_LIBRARY:
        return "INTERFACE_LIBRARY";
      case cmTarget::UNKNOWN_LIBRARY:
        return "UNKNOWN_LIBRARY";
    }
  assert(0 && "Unexpected target type");
  return 0;
}

//----------------------------------------------------------------------------
struct cmTarget::OutputInfo
{
  std::string OutDir;
  std::string ImpDir;
  std::string PdbDir;
};

//----------------------------------------------------------------------------
class cmTargetInternals
{
public:
  cmTargetInternals()
    {
    this->PolicyWarnedCMP0022 = false;
    this->SourceFileFlagsConstructed = false;
    }
  cmTargetInternals(cmTargetInternals const&)
    {
    this->PolicyWarnedCMP0022 = false;
    this->SourceFileFlagsConstructed = false;
    }
  ~cmTargetInternals();
  typedef cmTarget::SourceFileFlags SourceFileFlags;
  mutable std::map<cmSourceFile const*, SourceFileFlags> SourceFlagsMap;
  mutable bool SourceFileFlagsConstructed;

  // The backtrace when the target was created.
  cmListFileBacktrace Backtrace;

  // Cache link interface computation from each configuration.
  struct OptionalLinkInterface: public cmTarget::LinkInterface
  {
    OptionalLinkInterface(): Exists(false) {}
    bool Exists;
  };
  typedef std::map<TargetConfigPair, OptionalLinkInterface>
                                                          LinkInterfaceMapType;
  LinkInterfaceMapType LinkInterfaceMap;
  bool PolicyWarnedCMP0022;

  typedef std::map<cmStdString, cmTarget::OutputInfo> OutputInfoMapType;
  OutputInfoMapType OutputInfoMap;

  typedef std::map<TargetConfigPair, cmTarget::ImportInfo>
                                                            ImportInfoMapType;
  ImportInfoMapType ImportInfoMap;

  // Cache link implementation computation from each configuration.
  typedef std::map<TargetConfigPair,
                   cmTarget::LinkImplementation> LinkImplMapType;
  LinkImplMapType LinkImplMap;

  typedef cmGeneratorTarget::TargetPropertyEntry TargetPropertyEntry;

  std::vector<TargetPropertyEntry*> IncludeDirectoriesEntries;
  std::vector<TargetPropertyEntry*> CompileOptionsEntries;
  std::vector<TargetPropertyEntry*> CompileDefinitionsEntries;
  std::vector<cmValueWithOrigin> LinkImplementationPropertyEntries;

  mutable std::map<std::string, std::vector<TargetPropertyEntry*> >
                                CachedLinkInterfaceCompileOptionsEntries;
  mutable std::map<std::string, std::vector<TargetPropertyEntry*> >
                                CachedLinkInterfaceCompileDefinitionsEntries;

  mutable std::map<std::string, bool> CacheLinkInterfaceCompileDefinitionsDone;
  mutable std::map<std::string, bool> CacheLinkInterfaceCompileOptionsDone;
};

//----------------------------------------------------------------------------
const std::vector<cmValueWithOrigin>&
cmTarget::GetLinkImplementationPropertyEntries()
{
  return this->Internal->LinkImplementationPropertyEntries;
}

//----------------------------------------------------------------------------
const std::vector<cmGeneratorTarget::TargetPropertyEntry*>&
cmTarget::GetIncludeDirectoriesEntries()
{
  return this->Internal->IncludeDirectoriesEntries;
}

//----------------------------------------------------------------------------
void deleteAndClear(
      std::vector<cmTargetInternals::TargetPropertyEntry*> &entries)
{
  for (std::vector<cmTargetInternals::TargetPropertyEntry*>::const_iterator
      it = entries.begin(),
      end = entries.end();
      it != end; ++it)
    {
      delete *it;
    }
  entries.clear();
}

//----------------------------------------------------------------------------
void deleteAndClear(
  std::map<std::string,
          std::vector<cmTargetInternals::TargetPropertyEntry*> > &entries)
{
  for (std::map<std::string,
          std::vector<cmTargetInternals::TargetPropertyEntry*> >::iterator
        it = entries.begin(), end = entries.end(); it != end; ++it)
    {
    deleteAndClear(it->second);
    }
}

//----------------------------------------------------------------------------
cmTargetInternals::~cmTargetInternals()
{
  deleteAndClear(this->CachedLinkInterfaceCompileOptionsEntries);
  deleteAndClear(this->CachedLinkInterfaceCompileDefinitionsEntries);
}

//----------------------------------------------------------------------------
cmTarget::cmTarget()
{
#define INITIALIZE_TARGET_POLICY_MEMBER(POLICY) \
  this->PolicyStatus ## POLICY = cmPolicies::WARN;

  CM_FOR_EACH_TARGET_POLICY(INITIALIZE_TARGET_POLICY_MEMBER)

#undef INITIALIZE_TARGET_POLICY_MEMBER

  this->Makefile = 0;
  this->LinkLibrariesAnalyzed = false;
  this->HaveInstallRule = false;
  this->DLLPlatform = false;
  this->IsApple = false;
  this->IsImportedTarget = false;
  this->BuildInterfaceIncludesAppended = false;
  this->DebugCompileOptionsDone = false;
  this->DebugCompileDefinitionsDone = false;
}

//----------------------------------------------------------------------------
void cmTarget::DefineProperties(cmake *cm)
{
  cm->DefineProperty
    ("RULE_LAUNCH_COMPILE", cmProperty::TARGET,
     "", "", true);
  cm->DefineProperty
    ("RULE_LAUNCH_LINK", cmProperty::TARGET,
     "", "", true);
  cm->DefineProperty
    ("RULE_LAUNCH_CUSTOM", cmProperty::TARGET,
     "", "", true);
}

void cmTarget::SetType(TargetType type, const char* name)
{
  this->Name = name;
  // only add dependency information for library targets
  this->TargetTypeValue = type;
  if(this->TargetTypeValue >= STATIC_LIBRARY
     && this->TargetTypeValue <= MODULE_LIBRARY)
    {
    this->RecordDependencies = true;
    }
  else
    {
    this->RecordDependencies = false;
    }
}

//----------------------------------------------------------------------------
void cmTarget::SetMakefile(cmMakefile* mf)
{
  // Set our makefile.
  this->Makefile = mf;

  // set the cmake instance of the properties
  this->Properties.SetCMakeInstance(mf->GetCMakeInstance());

  // Check whether this is a DLL platform.
  this->DLLPlatform = (this->Makefile->IsOn("WIN32") ||
                       this->Makefile->IsOn("CYGWIN") ||
                       this->Makefile->IsOn("MINGW"));

  // Check whether we are targeting an Apple platform.
  this->IsApple = this->Makefile->IsOn("APPLE");

  // Setup default property values.
  if (this->GetType() != INTERFACE_LIBRARY)
    {
    this->SetPropertyDefault("INSTALL_NAME_DIR", 0);
    this->SetPropertyDefault("INSTALL_RPATH", "");
    this->SetPropertyDefault("INSTALL_RPATH_USE_LINK_PATH", "OFF");
    this->SetPropertyDefault("SKIP_BUILD_RPATH", "OFF");
    this->SetPropertyDefault("BUILD_WITH_INSTALL_RPATH", "OFF");
    this->SetPropertyDefault("ARCHIVE_OUTPUT_DIRECTORY", 0);
    this->SetPropertyDefault("LIBRARY_OUTPUT_DIRECTORY", 0);
    this->SetPropertyDefault("RUNTIME_OUTPUT_DIRECTORY", 0);
    this->SetPropertyDefault("PDB_OUTPUT_DIRECTORY", 0);
    this->SetPropertyDefault("Fortran_FORMAT", 0);
    this->SetPropertyDefault("Fortran_MODULE_DIRECTORY", 0);
    this->SetPropertyDefault("GNUtoMS", 0);
    this->SetPropertyDefault("OSX_ARCHITECTURES", 0);
    this->SetPropertyDefault("AUTOMOC", 0);
    this->SetPropertyDefault("AUTOUIC", 0);
    this->SetPropertyDefault("AUTORCC", 0);
    this->SetPropertyDefault("AUTOMOC_MOC_OPTIONS", 0);
    this->SetPropertyDefault("AUTOUIC_OPTIONS", 0);
    this->SetPropertyDefault("AUTORCC_OPTIONS", 0);
    this->SetPropertyDefault("LINK_DEPENDS_NO_SHARED", 0);
    this->SetPropertyDefault("LINK_INTERFACE_LIBRARIES", 0);
    this->SetPropertyDefault("WIN32_EXECUTABLE", 0);
    this->SetPropertyDefault("MACOSX_BUNDLE", 0);
    this->SetPropertyDefault("MACOSX_RPATH", 0);
    this->SetPropertyDefault("NO_SYSTEM_FROM_IMPORTED", 0);
    }

  // Collect the set of configuration types.
  std::vector<std::string> configNames;
  mf->GetConfigurations(configNames);

  // Setup per-configuration property default values.
  const char* configProps[] = {
    "ARCHIVE_OUTPUT_DIRECTORY_",
    "LIBRARY_OUTPUT_DIRECTORY_",
    "RUNTIME_OUTPUT_DIRECTORY_",
    "PDB_OUTPUT_DIRECTORY_",
    "MAP_IMPORTED_CONFIG_",
    0};
  for(std::vector<std::string>::iterator ci = configNames.begin();
      ci != configNames.end(); ++ci)
    {
    std::string configUpper = cmSystemTools::UpperCase(*ci);
    for(const char** p = configProps; *p; ++p)
      {
      if (this->TargetTypeValue == INTERFACE_LIBRARY
          && strcmp(*p, "MAP_IMPORTED_CONFIG_") != 0)
        {
        continue;
        }
      std::string property = *p;
      property += configUpper;
      this->SetPropertyDefault(property.c_str(), 0);
      }

    // Initialize per-configuration name postfix property from the
    // variable only for non-executable targets.  This preserves
    // compatibility with previous CMake versions in which executables
    // did not support this variable.  Projects may still specify the
    // property directly.  TODO: Make this depend on backwards
    // compatibility setting.
    if(this->TargetTypeValue != cmTarget::EXECUTABLE
        && this->TargetTypeValue != cmTarget::INTERFACE_LIBRARY)
      {
      std::string property = cmSystemTools::UpperCase(*ci);
      property += "_POSTFIX";
      this->SetPropertyDefault(property.c_str(), 0);
      }
    }

  // Save the backtrace of target construction.
  this->Makefile->GetBacktrace(this->Internal->Backtrace);

  if (!this->IsImported())
    {
    // Initialize the INCLUDE_DIRECTORIES property based on the current value
    // of the same directory property:
    const std::vector<cmValueWithOrigin> parentIncludes =
                                this->Makefile->GetIncludeDirectoriesEntries();

    for (std::vector<cmValueWithOrigin>::const_iterator it
                = parentIncludes.begin(); it != parentIncludes.end(); ++it)
      {
      this->InsertInclude(*it);
      }
    const std::set<cmStdString> parentSystemIncludes =
                                this->Makefile->GetSystemIncludeDirectories();

    for (std::set<cmStdString>::const_iterator it
          = parentSystemIncludes.begin();
          it != parentSystemIncludes.end(); ++it)
      {
      this->SystemIncludeDirectories.insert(*it);
      }

    const std::vector<cmValueWithOrigin> parentOptions =
                                this->Makefile->GetCompileOptionsEntries();

    for (std::vector<cmValueWithOrigin>::const_iterator it
                = parentOptions.begin(); it != parentOptions.end(); ++it)
      {
      this->InsertCompileOption(*it);
      }
    }

  if (this->GetType() != INTERFACE_LIBRARY)
    {
    this->SetPropertyDefault("C_VISIBILITY_PRESET", 0);
    this->SetPropertyDefault("CXX_VISIBILITY_PRESET", 0);
    this->SetPropertyDefault("VISIBILITY_INLINES_HIDDEN", 0);
    }

  if(this->TargetTypeValue == cmTarget::SHARED_LIBRARY
      || this->TargetTypeValue == cmTarget::MODULE_LIBRARY)
    {
    this->SetProperty("POSITION_INDEPENDENT_CODE", "True");
    }
  if (this->GetType() != INTERFACE_LIBRARY)
    {
    this->SetPropertyDefault("POSITION_INDEPENDENT_CODE", 0);
    }

  // Record current policies for later use.
#define CAPTURE_TARGET_POLICY(POLICY) \
  this->PolicyStatus ## POLICY = \
    this->Makefile->GetPolicyStatus(cmPolicies::POLICY);

  CM_FOR_EACH_TARGET_POLICY(CAPTURE_TARGET_POLICY)

#undef CAPTURE_TARGET_POLICY

  if (this->TargetTypeValue == INTERFACE_LIBRARY)
    {
    // This policy is checked in a few conditions. The properties relevant
    // to the policy are always ignored for INTERFACE_LIBRARY targets,
    // so ensure that the conditions don't lead to nonsense.
    this->PolicyStatusCMP0022 = cmPolicies::NEW;
    }

  this->SetPropertyDefault("JOB_POOL_COMPILE", 0);
  this->SetPropertyDefault("JOB_POOL_LINK", 0);
}

//----------------------------------------------------------------------------
void cmTarget::FinishConfigure()
{
  // Erase any cached link information that might have been comptued
  // on-demand during the configuration.  This ensures that build
  // system generation uses up-to-date information even if other cache
  // invalidation code in this source file is buggy.
  this->ClearLinkMaps();

  // Do old-style link dependency analysis.
  this->AnalyzeLibDependencies(*this->Makefile);
}

//----------------------------------------------------------------------------
void cmTarget::ClearLinkMaps()
{
  this->Internal->LinkImplMap.clear();
  this->Internal->LinkInterfaceMap.clear();
}

//----------------------------------------------------------------------------
cmListFileBacktrace const& cmTarget::GetBacktrace() const
{
  return this->Internal->Backtrace;
}

//----------------------------------------------------------------------------
std::string cmTarget::GetSupportDirectory() const
{
  std::string dir = this->Makefile->GetCurrentOutputDirectory();
  dir += cmake::GetCMakeFilesDirectory();
  dir += "/";
  dir += this->Name;
#if defined(__VMS)
  dir += "_dir";
#else
  dir += ".dir";
#endif
  return dir;
}

//----------------------------------------------------------------------------
bool cmTarget::IsExecutableWithExports() const
{
  return (this->GetType() == cmTarget::EXECUTABLE &&
          this->GetPropertyAsBool("ENABLE_EXPORTS"));
}

//----------------------------------------------------------------------------
bool cmTarget::IsLinkable() const
{
  return (this->GetType() == cmTarget::STATIC_LIBRARY ||
          this->GetType() == cmTarget::SHARED_LIBRARY ||
          this->GetType() == cmTarget::MODULE_LIBRARY ||
          this->GetType() == cmTarget::UNKNOWN_LIBRARY ||
          this->GetType() == cmTarget::INTERFACE_LIBRARY ||
          this->IsExecutableWithExports());
}

//----------------------------------------------------------------------------
bool cmTarget::HasImportLibrary() const
{
  return (this->DLLPlatform &&
          (this->GetType() == cmTarget::SHARED_LIBRARY ||
           this->IsExecutableWithExports()));
}

//----------------------------------------------------------------------------
bool cmTarget::IsFrameworkOnApple() const
{
  return (this->GetType() == cmTarget::SHARED_LIBRARY &&
          this->Makefile->IsOn("APPLE") &&
          this->GetPropertyAsBool("FRAMEWORK"));
}

//----------------------------------------------------------------------------
bool cmTarget::IsAppBundleOnApple() const
{
  return (this->GetType() == cmTarget::EXECUTABLE &&
          this->Makefile->IsOn("APPLE") &&
          this->GetPropertyAsBool("MACOSX_BUNDLE"));
}

//----------------------------------------------------------------------------
bool cmTarget::IsCFBundleOnApple() const
{
  return (this->GetType() == cmTarget::MODULE_LIBRARY &&
          this->Makefile->IsOn("APPLE") &&
          this->GetPropertyAsBool("BUNDLE"));
}

//----------------------------------------------------------------------------
bool cmTarget::IsBundleOnApple() const
{
  return this->IsFrameworkOnApple() || this->IsAppBundleOnApple() ||
         this->IsCFBundleOnApple();
}

//----------------------------------------------------------------------------
bool cmTarget::FindSourceFiles()
{
  for(std::vector<cmSourceFile*>::const_iterator
        si = this->SourceFiles.begin();
      si != this->SourceFiles.end(); ++si)
    {
    std::string e;
    if((*si)->GetFullPath(&e).empty())
      {
      if(!e.empty())
        {
        cmake* cm = this->Makefile->GetCMakeInstance();
        cm->IssueMessage(cmake::FATAL_ERROR, e,
                         this->GetBacktrace());
        }
      return false;
      }
    }
  return true;
}

//----------------------------------------------------------------------------
void cmTarget::GetSourceFiles(std::vector<cmSourceFile*> &files) const
{
  files = this->SourceFiles;
}

//----------------------------------------------------------------------------
void cmTarget::AddSourceFile(cmSourceFile* sf)
{
  if (std::find(this->SourceFiles.begin(), this->SourceFiles.end(), sf)
                                            == this->SourceFiles.end())
    {
    this->SourceFiles.push_back(sf);
    }
}

//----------------------------------------------------------------------------
void cmTarget::AddSources(std::vector<std::string> const& srcs)
{
  for(std::vector<std::string>::const_iterator i = srcs.begin();
      i != srcs.end(); ++i)
    {
    const char* src = i->c_str();
    if(src[0] == '$' && src[1] == '<')
      {
      this->ProcessSourceExpression(*i);
      }
    else
      {
      this->AddSource(src);
      }
    }
}

//----------------------------------------------------------------------------
cmSourceFile* cmTarget::AddSource(const char* s)
{
  std::string src = s;

  // For backwards compatibility replace varibles in source names.
  // This should eventually be removed.
  this->Makefile->ExpandVariablesInString(src);

  cmSourceFile* sf = this->Makefile->GetOrCreateSource(src.c_str());
  this->AddSourceFile(sf);
  return sf;
}

//----------------------------------------------------------------------------
void cmTarget::ProcessSourceExpression(std::string const& expr)
{
  if(cmHasLiteralPrefix(expr.c_str(), "$<TARGET_OBJECTS:") &&
     expr[expr.size()-1] == '>')
    {
    std::string objLibName = expr.substr(17, expr.size()-18);
    this->ObjectLibraries.push_back(objLibName);
    }
  else
    {
    cmOStringStream e;
    e << "Unrecognized generator expression:\n"
      << "  " << expr;
    this->Makefile->IssueMessage(cmake::FATAL_ERROR, e.str());
    }
}

//----------------------------------------------------------------------------
struct cmTarget::SourceFileFlags
cmTarget::GetTargetSourceFileFlags(const cmSourceFile* sf) const
{
  struct SourceFileFlags flags;
  this->ConstructSourceFileFlags();
  std::map<cmSourceFile const*, SourceFileFlags>::iterator si =
    this->Internal->SourceFlagsMap.find(sf);
  if(si != this->Internal->SourceFlagsMap.end())
    {
    flags = si->second;
    }
  return flags;
}

//----------------------------------------------------------------------------
void cmTarget::ConstructSourceFileFlags() const
{
  if(this->Internal->SourceFileFlagsConstructed)
    {
    return;
    }
  this->Internal->SourceFileFlagsConstructed = true;

  // Process public headers to mark the source files.
  if(const char* files = this->GetProperty("PUBLIC_HEADER"))
    {
    std::vector<std::string> relFiles;
    cmSystemTools::ExpandListArgument(files, relFiles);
    for(std::vector<std::string>::iterator it = relFiles.begin();
        it != relFiles.end(); ++it)
      {
      if(cmSourceFile* sf = this->Makefile->GetSource(it->c_str()))
        {
        SourceFileFlags& flags = this->Internal->SourceFlagsMap[sf];
        flags.MacFolder = "Headers";
        flags.Type = cmTarget::SourceFileTypePublicHeader;
        }
      }
    }

  // Process private headers after public headers so that they take
  // precedence if a file is listed in both.
  if(const char* files = this->GetProperty("PRIVATE_HEADER"))
    {
    std::vector<std::string> relFiles;
    cmSystemTools::ExpandListArgument(files, relFiles);
    for(std::vector<std::string>::iterator it = relFiles.begin();
        it != relFiles.end(); ++it)
      {
      if(cmSourceFile* sf = this->Makefile->GetSource(it->c_str()))
        {
        SourceFileFlags& flags = this->Internal->SourceFlagsMap[sf];
        flags.MacFolder = "PrivateHeaders";
        flags.Type = cmTarget::SourceFileTypePrivateHeader;
        }
      }
    }

  // Mark sources listed as resources.
  if(const char* files = this->GetProperty("RESOURCE"))
    {
    std::vector<std::string> relFiles;
    cmSystemTools::ExpandListArgument(files, relFiles);
    for(std::vector<std::string>::iterator it = relFiles.begin();
        it != relFiles.end(); ++it)
      {
      if(cmSourceFile* sf = this->Makefile->GetSource(it->c_str()))
        {
        SourceFileFlags& flags = this->Internal->SourceFlagsMap[sf];
        flags.MacFolder = "Resources";
        flags.Type = cmTarget::SourceFileTypeResource;
        }
      }
    }

  // Handle the MACOSX_PACKAGE_LOCATION property on source files that
  // were not listed in one of the other lists.
  std::vector<cmSourceFile*> sources;
  this->GetSourceFiles(sources);
  for(std::vector<cmSourceFile*>::const_iterator si = sources.begin();
      si != sources.end(); ++si)
    {
    cmSourceFile* sf = *si;
    if(const char* location = sf->GetProperty("MACOSX_PACKAGE_LOCATION"))
      {
      SourceFileFlags& flags = this->Internal->SourceFlagsMap[sf];
      if(flags.Type == cmTarget::SourceFileTypeNormal)
        {
        flags.MacFolder = location;
        if(strcmp(location, "Resources") == 0)
          {
          flags.Type = cmTarget::SourceFileTypeResource;
          }
        else
          {
          flags.Type = cmTarget::SourceFileTypeMacContent;
          }
        }
      }
    }
}

//----------------------------------------------------------------------------
void cmTarget::MergeLinkLibraries( cmMakefile& mf,
                                   const char *selfname,
                                   const LinkLibraryVectorType& libs )
{
  // Only add on libraries we haven't added on before.
  // Assumption: the global link libraries could only grow, never shrink
  LinkLibraryVectorType::const_iterator i = libs.begin();
  i += this->PrevLinkedLibraries.size();
  for( ; i != libs.end(); ++i )
    {
    // This is equivalent to the target_link_libraries plain signature.
    this->AddLinkLibrary( mf, selfname, i->first.c_str(), i->second );
    this->AppendProperty("INTERFACE_LINK_LIBRARIES",
      this->GetDebugGeneratorExpressions(i->first.c_str(), i->second).c_str());
    }
  this->PrevLinkedLibraries = libs;
}

//----------------------------------------------------------------------------
void cmTarget::AddLinkDirectory(const char* d)
{
  // Make sure we don't add unnecessary search directories.
  if(this->LinkDirectoriesEmmitted.insert(d).second)
    {
    this->LinkDirectories.push_back(d);
    }
}

//----------------------------------------------------------------------------
const std::vector<std::string>& cmTarget::GetLinkDirectories() const
{
  return this->LinkDirectories;
}

//----------------------------------------------------------------------------
cmTarget::LinkLibraryType cmTarget::ComputeLinkType(const char* config) const
{
  // No configuration is always optimized.
  if(!(config && *config))
    {
    return cmTarget::OPTIMIZED;
    }

  // Get the list of configurations considered to be DEBUG.
  std::vector<std::string> const& debugConfigs =
    this->Makefile->GetCMakeInstance()->GetDebugConfigs();

  // Check if any entry in the list matches this configuration.
  std::string configUpper = cmSystemTools::UpperCase(config);
  for(std::vector<std::string>::const_iterator i = debugConfigs.begin();
      i != debugConfigs.end(); ++i)
    {
    if(*i == configUpper)
      {
      return cmTarget::DEBUG;
      }
    }

  // The current configuration is not a debug configuration.
  return cmTarget::OPTIMIZED;
}

//----------------------------------------------------------------------------
void cmTarget::ClearDependencyInformation( cmMakefile& mf,
                                           const char* target )
{
  // Clear the dependencies. The cache variable must exist iff we are
  // recording dependency information for this target.
  std::string depname = target;
  depname += "_LIB_DEPENDS";
  if (this->RecordDependencies)
    {
    mf.AddCacheDefinition(depname.c_str(), "",
                          "Dependencies for target", cmCacheManager::STATIC);
    }
  else
    {
    if (mf.GetDefinition( depname.c_str() ))
      {
      std::string message = "Target ";
      message += target;
      message += " has dependency information when it shouldn't.\n";
      message += "Your cache is probably stale. Please remove the entry\n  ";
      message += depname;
      message += "\nfrom the cache.";
      cmSystemTools::Error( message.c_str() );
      }
    }
}

//----------------------------------------------------------------------------
bool cmTarget::NameResolvesToFramework(const std::string& libname) const
{
  return this->Makefile->GetLocalGenerator()->GetGlobalGenerator()->
    NameResolvesToFramework(libname);
}

//----------------------------------------------------------------------------
void cmTarget::GetDirectLinkLibraries(const char *config,
                            std::vector<std::string> &libs,
                            cmTarget const* head) const
{
  const char *prop = this->GetProperty("LINK_LIBRARIES");
  if (prop)
    {
    cmListFileBacktrace lfbt;
    cmGeneratorExpression ge(lfbt);
    const cmsys::auto_ptr<cmCompiledGeneratorExpression> cge = ge.Parse(prop);

    cmGeneratorExpressionDAGChecker dagChecker(lfbt,
                                        this->GetName(),
                                        "LINK_LIBRARIES", 0, 0);
    cmSystemTools::ExpandListArgument(cge->Evaluate(this->Makefile,
                                        config,
                                        false,
                                        head,
                                        &dagChecker),
                                      libs);

    std::set<cmStdString> seenProps = cge->GetSeenTargetProperties();
    for (std::set<cmStdString>::const_iterator it = seenProps.begin();
        it != seenProps.end(); ++it)
      {
      if (!this->GetProperty(it->c_str()))
        {
        this->LinkImplicitNullProperties.insert(*it);
        }
      }
    }
}

//----------------------------------------------------------------------------
void cmTarget::GetInterfaceLinkLibraries(const char *config,
                                         std::vector<std::string> &libs,
                                         cmTarget const* head) const
{
  const char *prop = this->GetProperty("INTERFACE_LINK_LIBRARIES");
  if (prop)
    {
    cmListFileBacktrace lfbt;
    cmGeneratorExpression ge(lfbt);
    const cmsys::auto_ptr<cmCompiledGeneratorExpression> cge = ge.Parse(prop);

    cmGeneratorExpressionDAGChecker dagChecker(lfbt,
                                        this->GetName(),
                                        "INTERFACE_LINK_LIBRARIES", 0, 0);
    cmSystemTools::ExpandListArgument(cge->Evaluate(this->Makefile,
                                        config,
                                        false,
                                        head,
                                        &dagChecker),
                                      libs);
    }
}

//----------------------------------------------------------------------------
std::string cmTarget::GetDebugGeneratorExpressions(const std::string &value,
                                  cmTarget::LinkLibraryType llt) const
{
  if (llt == GENERAL)
    {
    return value;
    }

  // Get the list of configurations considered to be DEBUG.
  std::vector<std::string> const& debugConfigs =
                      this->Makefile->GetCMakeInstance()->GetDebugConfigs();

  std::string configString = "$<CONFIG:" + debugConfigs[0] + ">";

  if (debugConfigs.size() > 1)
    {
    for(std::vector<std::string>::const_iterator
          li = debugConfigs.begin() + 1; li != debugConfigs.end(); ++li)
      {
      configString += ",$<CONFIG:" + *li + ">";
      }
    configString = "$<OR:" + configString + ">";
    }

  if (llt == OPTIMIZED)
    {
    configString = "$<NOT:" + configString + ">";
    }
  return "$<" + configString + ":" + value + ">";
}

//----------------------------------------------------------------------------
static std::string targetNameGenex(const char *lib)
{
  return std::string("$<TARGET_NAME:") + lib + ">";
}

//----------------------------------------------------------------------------
bool cmTarget::PushTLLCommandTrace(TLLSignature signature)
{
  bool ret = true;
  if (!this->TLLCommands.empty())
    {
    if (this->TLLCommands.back().first != signature)
      {
      ret = false;
      }
    }
  cmListFileBacktrace lfbt;
  this->Makefile->GetBacktrace(lfbt);
  this->TLLCommands.push_back(std::make_pair(signature, lfbt));
  return ret;
}

//----------------------------------------------------------------------------
void cmTarget::GetTllSignatureTraces(cmOStringStream &s,
                                     TLLSignature sig) const
{
  std::vector<cmListFileBacktrace> sigs;
  typedef std::vector<std::pair<TLLSignature, cmListFileBacktrace> > Container;
  for(Container::const_iterator it = this->TLLCommands.begin();
      it != this->TLLCommands.end(); ++it)
    {
    if (it->first == sig)
      {
      sigs.push_back(it->second);
      }
    }
  if (!sigs.empty())
    {
    const char *sigString
                        = (sig == cmTarget::KeywordTLLSignature ? "keyword"
                                                                : "plain");
    s << "The uses of the " << sigString << " signature are here:\n";
    std::set<cmStdString> emitted;
    for(std::vector<cmListFileBacktrace>::const_iterator it = sigs.begin();
        it != sigs.end(); ++it)
      {
      cmListFileBacktrace::const_iterator i = it->begin();
      if(i != it->end())
        {
        cmListFileContext const& lfc = *i;
        cmOStringStream line;
        line << " * " << (lfc.Line? "": " in ") << lfc << std::endl;
        if (emitted.insert(line.str()).second)
          {
          s << line.str();
          }
        ++i;
        }
      }
    }
}

//----------------------------------------------------------------------------
void cmTarget::AddLinkLibrary(cmMakefile& mf,
                              const char *target, const char* lib,
                              LinkLibraryType llt)
{
  cmTarget *tgt = this->Makefile->FindTargetToUse(lib);
  {
  const bool isNonImportedTarget = tgt && !tgt->IsImported();

  const std::string libName = (isNonImportedTarget && llt != GENERAL)
                                                        ? targetNameGenex(lib)
                                                        : std::string(lib);
  this->AppendProperty("LINK_LIBRARIES",
                       this->GetDebugGeneratorExpressions(libName,
                                                          llt).c_str());
  }

  if (cmGeneratorExpression::Find(lib) != std::string::npos
      || (tgt && tgt->GetType() == INTERFACE_LIBRARY)
      || (strcmp( target, lib ) == 0))
    {
    return;
    }

  cmTarget::LibraryID tmp;
  tmp.first = lib;
  tmp.second = llt;
  this->LinkLibraries.push_back( tmp );
  this->OriginalLinkLibraries.push_back(tmp);
  this->ClearLinkMaps();

  // Add the explicit dependency information for this target. This is
  // simply a set of libraries separated by ";". There should always
  // be a trailing ";". These library names are not canonical, in that
  // they may be "-framework x", "-ly", "/path/libz.a", etc.
  // We shouldn't remove duplicates here because external libraries
  // may be purposefully duplicated to handle recursive dependencies,
  // and we removing one instance will break the link line. Duplicates
  // will be appropriately eliminated at emit time.
  if(this->RecordDependencies)
    {
    std::string targetEntry = target;
    targetEntry += "_LIB_DEPENDS";
    std::string dependencies;
    const char* old_val = mf.GetDefinition( targetEntry.c_str() );
    if( old_val )
      {
      dependencies += old_val;
      }
    switch (llt)
      {
      case cmTarget::GENERAL:
        dependencies += "general";
        break;
      case cmTarget::DEBUG:
        dependencies += "debug";
        break;
      case cmTarget::OPTIMIZED:
        dependencies += "optimized";
        break;
      }
    dependencies += ";";
    dependencies += lib;
    dependencies += ";";
    mf.AddCacheDefinition( targetEntry.c_str(), dependencies.c_str(),
                           "Dependencies for the target",
                           cmCacheManager::STATIC );
    }

}

//----------------------------------------------------------------------------
void
cmTarget::AddSystemIncludeDirectories(const std::set<cmStdString> &incs)
{
  for(std::set<cmStdString>::const_iterator li = incs.begin();
      li != incs.end(); ++li)
    {
    this->SystemIncludeDirectories.insert(*li);
    }
}

//----------------------------------------------------------------------------
void
cmTarget::AddSystemIncludeDirectories(const std::vector<std::string> &incs)
{
  for(std::vector<std::string>::const_iterator li = incs.begin();
      li != incs.end(); ++li)
    {
    this->SystemIncludeDirectories.insert(*li);
    }
}

//----------------------------------------------------------------------------
void
cmTarget::AnalyzeLibDependencies( const cmMakefile& mf )
{
  // There are two key parts of the dependency analysis: (1)
  // determining the libraries in the link line, and (2) constructing
  // the dependency graph for those libraries.
  //
  // The latter is done using the cache entries that record the
  // dependencies of each library.
  //
  // The former is a more thorny issue, since it is not clear how to
  // determine if two libraries listed on the link line refer to the a
  // single library or not. For example, consider the link "libraries"
  //    /usr/lib/libtiff.so -ltiff
  // Is this one library or two? The solution implemented here is the
  // simplest (and probably the only practical) one: two libraries are
  // the same if their "link strings" are identical. Thus, the two
  // libraries above are considered distinct. This also means that for
  // dependency analysis to be effective, the CMake user must specify
  // libraries build by his project without using any linker flags or
  // file extensions. That is,
  //    LINK_LIBRARIES( One Two )
  // instead of
  //    LINK_LIBRARIES( -lOne ${binarypath}/libTwo.a )
  // The former is probably what most users would do, but it never
  // hurts to document the assumptions. :-) Therefore, in the analysis
  // code, the "canonical name" of a library is simply its name as
  // given to a LINK_LIBRARIES command.
  //
  // Also, we will leave the original link line intact; we will just add any
  // dependencies that were missing.
  //
  // There is a problem with recursive external libraries
  // (i.e. libraries with no dependency information that are
  // recursively dependent). We must make sure that the we emit one of
  // the libraries twice to satisfy the recursion, but we shouldn't
  // emit it more times than necessary. In particular, we must make
  // sure that handling this improbable case doesn't cost us when
  // dealing with the common case of non-recursive libraries. The
  // solution is to assume that the recursion is satisfied at one node
  // of the dependency tree. To illustrate, assume libA and libB are
  // extrenal and mutually dependent. Suppose libX depends on
  // libA, and libY on libA and libX. Then
  //   TARGET_LINK_LIBRARIES( Y X A B A )
  //   TARGET_LINK_LIBRARIES( X A B A )
  //   TARGET_LINK_LIBRARIES( Exec Y )
  // would result in "-lY -lX -lA -lB -lA". This is the correct way to
  // specify the dependencies, since the mutual dependency of A and B
  // is resolved *every time libA is specified*.
  //
  // Something like
  //   TARGET_LINK_LIBRARIES( Y X A B A )
  //   TARGET_LINK_LIBRARIES( X A B )
  //   TARGET_LINK_LIBRARIES( Exec Y )
  // would result in "-lY -lX -lA -lB", and the mutual dependency
  // information is lost. This is because in some case (Y), the mutual
  // dependency of A and B is listed, while in another other case (X),
  // it is not. Depending on which line actually emits A, the mutual
  // dependency may or may not be on the final link line.  We can't
  // handle this pathalogical case cleanly without emitting extra
  // libraries for the normal cases. Besides, the dependency
  // information for X is wrong anyway: if we build an executable
  // depending on X alone, we would not have the mutual dependency on
  // A and B resolved.
  //
  // IMPROVEMENTS:
  // -- The current algorithm will not always pick the "optimal" link line
  //    when recursive dependencies are present. It will instead break the
  //    cycles at an aribtrary point. The majority of projects won't have
  //    cyclic dependencies, so this is probably not a big deal. Note that
  //    the link line is always correct, just not necessary optimal.

 {
 // Expand variables in link library names.  This is for backwards
 // compatibility with very early CMake versions and should
 // eventually be removed.  This code was moved here from the end of
 // old source list processing code which was called just before this
 // method.
 for(LinkLibraryVectorType::iterator p = this->LinkLibraries.begin();
     p != this->LinkLibraries.end(); ++p)
   {
   this->Makefile->ExpandVariablesInString(p->first, true, true);
   }
 }

 // The dependency map.
 DependencyMap dep_map;

 // 1. Build the dependency graph
 //
 for(LinkLibraryVectorType::reverse_iterator lib
       = this->LinkLibraries.rbegin();
     lib != this->LinkLibraries.rend(); ++lib)
   {
   this->GatherDependencies( mf, *lib, dep_map);
   }

 // 2. Remove any dependencies that are already satisfied in the original
 // link line.
 //
 for(LinkLibraryVectorType::iterator lib = this->LinkLibraries.begin();
     lib != this->LinkLibraries.end(); ++lib)
   {
   for( LinkLibraryVectorType::iterator lib2 = lib;
        lib2 != this->LinkLibraries.end(); ++lib2)
     {
     this->DeleteDependency( dep_map, *lib, *lib2);
     }
   }


 // 3. Create the new link line by simply emitting any dependencies that are
 // missing.  Start from the back and keep adding.
 //
 std::set<DependencyMap::key_type> done, visited;
 std::vector<DependencyMap::key_type> newLinkLibraries;
 for(LinkLibraryVectorType::reverse_iterator lib =
       this->LinkLibraries.rbegin();
     lib != this->LinkLibraries.rend(); ++lib)
   {
   // skip zero size library entries, this may happen
   // if a variable expands to nothing.
   if (lib->first.size() != 0)
     {
     this->Emit( *lib, dep_map, done, visited, newLinkLibraries );
     }
   }

 // 4. Add the new libraries to the link line.
 //
 for( std::vector<DependencyMap::key_type>::reverse_iterator k =
        newLinkLibraries.rbegin();
      k != newLinkLibraries.rend(); ++k )
   {
   // get the llt from the dep_map
   this->LinkLibraries.push_back( std::make_pair(k->first,k->second) );
   }
 this->LinkLibrariesAnalyzed = true;
}

//----------------------------------------------------------------------------
void cmTarget::InsertDependency( DependencyMap& depMap,
                                 const LibraryID& lib,
                                 const LibraryID& dep)
{
  depMap[lib].push_back(dep);
}

//----------------------------------------------------------------------------
void cmTarget::DeleteDependency( DependencyMap& depMap,
                                 const LibraryID& lib,
                                 const LibraryID& dep)
{
  // Make sure there is an entry in the map for lib. If so, delete all
  // dependencies to dep. There may be repeated entries because of
  // external libraries that are specified multiple times.
  DependencyMap::iterator map_itr = depMap.find( lib );
  if( map_itr != depMap.end() )
    {
    DependencyList& depList = map_itr->second;
    DependencyList::iterator itr;
    while( (itr = std::find(depList.begin(), depList.end(), dep)) !=
           depList.end() )
      {
      depList.erase( itr );
      }
    }
}

//----------------------------------------------------------------------------
void cmTarget::Emit(const LibraryID lib,
                    const DependencyMap& dep_map,
                    std::set<LibraryID>& emitted,
                    std::set<LibraryID>& visited,
                    DependencyList& link_line )
{
  // It's already been emitted
  if( emitted.find(lib) != emitted.end() )
    {
    return;
    }

  // Emit the dependencies only if this library node hasn't been
  // visited before. If it has, then we have a cycle. The recursion
  // that got us here should take care of everything.

  if( visited.insert(lib).second )
    {
    if( dep_map.find(lib) != dep_map.end() ) // does it have dependencies?
      {
      const DependencyList& dep_on = dep_map.find( lib )->second;
      DependencyList::const_reverse_iterator i;

      // To cater for recursive external libraries, we must emit
      // duplicates on this link line *unless* they were emitted by
      // some other node, in which case we assume that the recursion
      // was resolved then. We making the simplifying assumption that
      // any duplicates on a single link line are on purpose, and must
      // be preserved.

      // This variable will keep track of the libraries that were
      // emitted directly from the current node, and not from a
      // recursive call. This way, if we come across a library that
      // has already been emitted, we repeat it iff it has been
      // emitted here.
      std::set<DependencyMap::key_type> emitted_here;
      for( i = dep_on.rbegin(); i != dep_on.rend(); ++i )
        {
        if( emitted_here.find(*i) != emitted_here.end() )
          {
          // a repeat. Must emit.
          emitted.insert(*i);
          link_line.push_back( *i );
          }
        else
          {
          // Emit only if no-one else has
          if( emitted.find(*i) == emitted.end() )
            {
            // emit dependencies
            Emit( *i, dep_map, emitted, visited, link_line );
            // emit self
            emitted.insert(*i);
            emitted_here.insert(*i);
            link_line.push_back( *i );
            }
          }
        }
      }
    }
}

//----------------------------------------------------------------------------
void cmTarget::GatherDependencies( const cmMakefile& mf,
                                   const LibraryID& lib,
                                   DependencyMap& dep_map)
{
  // If the library is already in the dependency map, then it has
  // already been fully processed.
  if( dep_map.find(lib) != dep_map.end() )
    {
    return;
    }

  const char* deps = mf.GetDefinition( (lib.first+"_LIB_DEPENDS").c_str() );
  if( deps && strcmp(deps,"") != 0 )
    {
    // Make sure this library is in the map, even if it has an empty
    // set of dependencies. This distinguishes the case of explicitly
    // no dependencies with that of unspecified dependencies.
    dep_map[lib];

    // Parse the dependency information, which is a set of
    // type, library pairs separated by ";". There is always a trailing ";".
    cmTarget::LinkLibraryType llt = cmTarget::GENERAL;
    std::string depline = deps;
    std::string::size_type start = 0;
    std::string::size_type end;
    end = depline.find( ";", start );
    while( end != std::string::npos )
      {
      std::string l = depline.substr( start, end-start );
      if( l.size() != 0 )
        {
        if (l == "debug")
          {
          llt = cmTarget::DEBUG;
          }
        else if (l == "optimized")
          {
          llt = cmTarget::OPTIMIZED;
          }
        else if (l == "general")
          {
          llt = cmTarget::GENERAL;
          }
        else
          {
          LibraryID lib2(l,llt);
          this->InsertDependency( dep_map, lib, lib2);
          this->GatherDependencies( mf, lib2, dep_map);
          llt = cmTarget::GENERAL;
          }
        }
      start = end+1; // skip the ;
      end = depline.find( ";", start );
      }
    // cannot depend on itself
    this->DeleteDependency( dep_map, lib, lib);
    }
}

//----------------------------------------------------------------------------
static bool whiteListedInterfaceProperty(const char *prop)
{
  if(cmHasLiteralPrefix(prop, "INTERFACE_"))
    {
    return true;
    }
  static const char* builtIns[] = {
    // ###: This must remain sorted. It is processed with a binary search.
    "COMPATIBLE_INTERFACE_BOOL",
    "COMPATIBLE_INTERFACE_NUMBER_MAX",
    "COMPATIBLE_INTERFACE_NUMBER_MIN",
    "COMPATIBLE_INTERFACE_STRING",
    "EXPORT_NAME",
    "IMPORTED",
    "NAME",
    "TYPE"
  };

  if (std::binary_search(cmArrayBegin(builtIns),
                         cmArrayEnd(builtIns),
                         prop,
                         cmStrCmp(prop)))
    {
    return true;
    }

  if (cmHasLiteralPrefix(prop, "MAP_IMPORTED_CONFIG_"))
    {
    return true;
    }

  return false;
}

//----------------------------------------------------------------------------
void cmTarget::SetProperty(const char* prop, const char* value)
{
  if (!prop)
    {
    return;
    }
  if (this->GetType() == INTERFACE_LIBRARY
      && !whiteListedInterfaceProperty(prop))
    {
    cmOStringStream e;
    e << "INTERFACE_LIBRARY targets may only have whitelisted properties.  "
         "The property \"" << prop << "\" is not allowed.";
    this->Makefile->IssueMessage(cmake::FATAL_ERROR, e.str().c_str());
    return;
    }

  if (strcmp(prop, "NAME") == 0)
    {
    cmOStringStream e;
    e << "NAME property is read-only\n";
    this->Makefile->IssueMessage(cmake::FATAL_ERROR, e.str().c_str());
    return;
    }
  if(strcmp(prop,"INCLUDE_DIRECTORIES") == 0)
    {
    cmListFileBacktrace lfbt;
    this->Makefile->GetBacktrace(lfbt);
    cmGeneratorExpression ge(lfbt);
    deleteAndClear(this->Internal->IncludeDirectoriesEntries);
    cmsys::auto_ptr<cmCompiledGeneratorExpression> cge = ge.Parse(value);
    this->Internal->IncludeDirectoriesEntries.push_back(
                          new cmTargetInternals::TargetPropertyEntry(cge));
    return;
    }
  if(strcmp(prop,"COMPILE_OPTIONS") == 0)
    {
    cmListFileBacktrace lfbt;
    this->Makefile->GetBacktrace(lfbt);
    cmGeneratorExpression ge(lfbt);
    deleteAndClear(this->Internal->CompileOptionsEntries);
    cmsys::auto_ptr<cmCompiledGeneratorExpression> cge = ge.Parse(value);
    this->Internal->CompileOptionsEntries.push_back(
                          new cmTargetInternals::TargetPropertyEntry(cge));
    return;
    }
  if(strcmp(prop,"COMPILE_DEFINITIONS") == 0)
    {
    cmListFileBacktrace lfbt;
    this->Makefile->GetBacktrace(lfbt);
    cmGeneratorExpression ge(lfbt);
    deleteAndClear(this->Internal->CompileDefinitionsEntries);
    cmsys::auto_ptr<cmCompiledGeneratorExpression> cge = ge.Parse(value);
    this->Internal->CompileDefinitionsEntries.push_back(
                          new cmTargetInternals::TargetPropertyEntry(cge));
    return;
    }
  if(strcmp(prop,"EXPORT_NAME") == 0 && this->IsImported())
    {
    cmOStringStream e;
    e << "EXPORT_NAME property can't be set on imported targets (\""
          << this->Name << "\")\n";
    this->Makefile->IssueMessage(cmake::FATAL_ERROR, e.str().c_str());
    return;
    }
  if (strcmp(prop, "LINK_LIBRARIES") == 0)
    {
    this->Internal->LinkImplementationPropertyEntries.clear();
    cmListFileBacktrace lfbt;
    this->Makefile->GetBacktrace(lfbt);
    cmValueWithOrigin entry(value, lfbt);
    this->Internal->LinkImplementationPropertyEntries.push_back(entry);
    return;
    }
  this->Properties.SetProperty(prop, value, cmProperty::TARGET);
  this->MaybeInvalidatePropertyCache(prop);
}

//----------------------------------------------------------------------------
void cmTarget::AppendProperty(const char* prop, const char* value,
                              bool asString)
{
  if (!prop)
    {
    return;
    }
  if (this->GetType() == INTERFACE_LIBRARY
      && !whiteListedInterfaceProperty(prop))
    {
    cmOStringStream e;
    e << "INTERFACE_LIBRARY targets may only have whitelisted properties.  "
         "The property \"" << prop << "\" is not allowed.";
    this->Makefile->IssueMessage(cmake::FATAL_ERROR, e.str().c_str());
    return;
    }
  if (strcmp(prop, "NAME") == 0)
    {
    cmOStringStream e;
    e << "NAME property is read-only\n";
    this->Makefile->IssueMessage(cmake::FATAL_ERROR, e.str().c_str());
    return;
    }
  if(strcmp(prop,"INCLUDE_DIRECTORIES") == 0)
    {
    cmListFileBacktrace lfbt;
    this->Makefile->GetBacktrace(lfbt);
    cmGeneratorExpression ge(lfbt);
    this->Internal->IncludeDirectoriesEntries.push_back(
              new cmTargetInternals::TargetPropertyEntry(ge.Parse(value)));
    return;
    }
  if(strcmp(prop,"COMPILE_OPTIONS") == 0)
    {
    cmListFileBacktrace lfbt;
    this->Makefile->GetBacktrace(lfbt);
    cmGeneratorExpression ge(lfbt);
    this->Internal->CompileOptionsEntries.push_back(
              new cmTargetInternals::TargetPropertyEntry(ge.Parse(value)));
    return;
    }
  if(strcmp(prop,"COMPILE_DEFINITIONS") == 0)
    {
    cmListFileBacktrace lfbt;
    this->Makefile->GetBacktrace(lfbt);
    cmGeneratorExpression ge(lfbt);
    this->Internal->CompileDefinitionsEntries.push_back(
              new cmTargetInternals::TargetPropertyEntry(ge.Parse(value)));
    return;
    }
  if(strcmp(prop,"EXPORT_NAME") == 0 && this->IsImported())
    {
    cmOStringStream e;
    e << "EXPORT_NAME property can't be set on imported targets (\""
          << this->Name << "\")\n";
    this->Makefile->IssueMessage(cmake::FATAL_ERROR, e.str().c_str());
    return;
    }
  if (strcmp(prop, "LINK_LIBRARIES") == 0)
    {
    cmListFileBacktrace lfbt;
    this->Makefile->GetBacktrace(lfbt);
    cmValueWithOrigin entry(value, lfbt);
    this->Internal->LinkImplementationPropertyEntries.push_back(entry);
    return;
    }
  this->Properties.AppendProperty(prop, value, cmProperty::TARGET, asString);
  this->MaybeInvalidatePropertyCache(prop);
}

//----------------------------------------------------------------------------
const char* cmTarget::GetExportName() const
{
  const char *exportName = this->GetProperty("EXPORT_NAME");

  if (exportName && *exportName)
    {
    if (!cmGeneratorExpression::IsValidTargetName(exportName))
      {
      cmOStringStream e;
      e << "EXPORT_NAME property \"" << exportName << "\" for \""
        << this->GetName() << "\": is not valid.";
      cmSystemTools::Error(e.str().c_str());
      return "";
      }
    return exportName;
    }
  return this->GetName();
}

//----------------------------------------------------------------------------
void cmTarget::AppendBuildInterfaceIncludes()
{
  if(this->GetType() != cmTarget::SHARED_LIBRARY &&
     this->GetType() != cmTarget::STATIC_LIBRARY &&
     this->GetType() != cmTarget::MODULE_LIBRARY &&
     this->GetType() != cmTarget::INTERFACE_LIBRARY &&
     !this->IsExecutableWithExports())
    {
    return;
    }
  if (this->BuildInterfaceIncludesAppended)
    {
    return;
    }
  this->BuildInterfaceIncludesAppended = true;

  if (this->Makefile->IsOn("CMAKE_INCLUDE_CURRENT_DIR_IN_INTERFACE"))
    {
    const char *binDir = this->Makefile->GetStartOutputDirectory();
    const char *srcDir = this->Makefile->GetStartDirectory();
    const std::string dirs = std::string(binDir ? binDir : "")
                            + std::string(binDir ? ";" : "")
                            + std::string(srcDir ? srcDir : "");
    if (!dirs.empty())
      {
      this->AppendProperty("INTERFACE_INCLUDE_DIRECTORIES",
                            ("$<BUILD_INTERFACE:" + dirs + ">").c_str());
      }
    }
}

//----------------------------------------------------------------------------
void cmTarget::InsertInclude(const cmValueWithOrigin &entry,
                     bool before)
{
  cmGeneratorExpression ge(entry.Backtrace);

  std::vector<cmTargetInternals::TargetPropertyEntry*>::iterator position
                = before ? this->Internal->IncludeDirectoriesEntries.begin()
                         : this->Internal->IncludeDirectoriesEntries.end();

  this->Internal->IncludeDirectoriesEntries.insert(position,
      new cmTargetInternals::TargetPropertyEntry(ge.Parse(entry.Value)));
}

//----------------------------------------------------------------------------
void cmTarget::InsertCompileOption(const cmValueWithOrigin &entry,
                     bool before)
{
  cmGeneratorExpression ge(entry.Backtrace);

  std::vector<cmTargetInternals::TargetPropertyEntry*>::iterator position
                = before ? this->Internal->CompileOptionsEntries.begin()
                         : this->Internal->CompileOptionsEntries.end();

  this->Internal->CompileOptionsEntries.insert(position,
      new cmTargetInternals::TargetPropertyEntry(ge.Parse(entry.Value)));
}

//----------------------------------------------------------------------------
void cmTarget::InsertCompileDefinition(const cmValueWithOrigin &entry,
                     bool before)
{
  cmGeneratorExpression ge(entry.Backtrace);

  std::vector<cmTargetInternals::TargetPropertyEntry*>::iterator position
                = before ? this->Internal->CompileDefinitionsEntries.begin()
                         : this->Internal->CompileDefinitionsEntries.end();

  this->Internal->CompileDefinitionsEntries.insert(position,
      new cmTargetInternals::TargetPropertyEntry(ge.Parse(entry.Value)));
}

//----------------------------------------------------------------------------
static void processCompileOptionsInternal(cmTarget const* tgt,
      const std::vector<cmTargetInternals::TargetPropertyEntry*> &entries,
      std::vector<std::string> &options,
      std::set<std::string> &uniqueOptions,
      cmGeneratorExpressionDAGChecker *dagChecker,
      const char *config, bool debugOptions, const char *logName)
{
  cmMakefile *mf = tgt->GetMakefile();

  for (std::vector<cmTargetInternals::TargetPropertyEntry*>::const_iterator
      it = entries.begin(), end = entries.end(); it != end; ++it)
    {
    bool cacheOptions = false;
    std::vector<std::string> entryOptions = (*it)->CachedEntries;
    if(entryOptions.empty())
      {
      cmSystemTools::ExpandListArgument((*it)->ge->Evaluate(mf,
                                                config,
                                                false,
                                                tgt,
                                                dagChecker),
                                      entryOptions);
      if (mf->IsGeneratingBuildSystem()
          && !(*it)->ge->GetHadContextSensitiveCondition())
        {
        cacheOptions = true;
        }
      }
    std::string usedOptions;
    for(std::vector<std::string>::iterator
          li = entryOptions.begin(); li != entryOptions.end(); ++li)
      {
      std::string opt = *li;

      if(uniqueOptions.insert(opt).second)
        {
        options.push_back(opt);
        if (debugOptions)
          {
          usedOptions += " * " + opt + "\n";
          }
        }
      }
    if (cacheOptions)
      {
      (*it)->CachedEntries = entryOptions;
      }
    if (!usedOptions.empty())
      {
      mf->GetCMakeInstance()->IssueMessage(cmake::LOG,
                            std::string("Used compile ") + logName
                            + std::string(" for target ")
                            + tgt->GetName() + ":\n"
                            + usedOptions, (*it)->ge->GetBacktrace());
      }
    }
}

//----------------------------------------------------------------------------
static void processCompileOptions(cmTarget const* tgt,
      const std::vector<cmTargetInternals::TargetPropertyEntry*> &entries,
      std::vector<std::string> &options,
      std::set<std::string> &uniqueOptions,
      cmGeneratorExpressionDAGChecker *dagChecker,
      const char *config, bool debugOptions)
{
  processCompileOptionsInternal(tgt, entries, options, uniqueOptions,
                                dagChecker, config, debugOptions, "options");
}

//----------------------------------------------------------------------------
void cmTarget::GetCompileOptions(std::vector<std::string> &result,
                                 const char *config) const
{
  std::set<std::string> uniqueOptions;
  cmListFileBacktrace lfbt;

  cmGeneratorExpressionDAGChecker dagChecker(lfbt,
                                              this->GetName(),
                                              "COMPILE_OPTIONS", 0, 0);

  std::vector<std::string> debugProperties;
  const char *debugProp =
              this->Makefile->GetDefinition("CMAKE_DEBUG_TARGET_PROPERTIES");
  if (debugProp)
    {
    cmSystemTools::ExpandListArgument(debugProp, debugProperties);
    }

  bool debugOptions = !this->DebugCompileOptionsDone
                    && std::find(debugProperties.begin(),
                                 debugProperties.end(),
                                 "COMPILE_OPTIONS")
                        != debugProperties.end();

  if (this->Makefile->IsGeneratingBuildSystem())
    {
    this->DebugCompileOptionsDone = true;
    }

  processCompileOptions(this,
                            this->Internal->CompileOptionsEntries,
                            result,
                            uniqueOptions,
                            &dagChecker,
                            config,
                            debugOptions);

  std::string configString = config ? config : "";
  if (!this->Internal->CacheLinkInterfaceCompileOptionsDone[configString])
    {
    for (std::vector<cmValueWithOrigin>::const_iterator
        it = this->Internal->LinkImplementationPropertyEntries.begin(),
        end = this->Internal->LinkImplementationPropertyEntries.end();
        it != end; ++it)
      {
      if (!cmGeneratorExpression::IsValidTargetName(it->Value)
          && cmGeneratorExpression::Find(it->Value) == std::string::npos)
        {
        continue;
        }
      {
      cmGeneratorExpression ge(lfbt);
      cmsys::auto_ptr<cmCompiledGeneratorExpression> cge =
                                                        ge.Parse(it->Value);
      std::string targetResult = cge->Evaluate(this->Makefile, config,
                                        false, this, 0, 0);
      if (!this->Makefile->FindTargetToUse(targetResult.c_str()))
        {
        continue;
        }
      }
      std::string optionGenex = "$<TARGET_PROPERTY:" +
                              it->Value + ",INTERFACE_COMPILE_OPTIONS>";
      if (cmGeneratorExpression::Find(it->Value) != std::string::npos)
        {
        // Because it->Value is a generator expression, ensure that it
        // evaluates to the non-empty string before being used in the
        // TARGET_PROPERTY expression.
        optionGenex = "$<$<BOOL:" + it->Value + ">:" + optionGenex + ">";
        }
      cmGeneratorExpression ge(it->Backtrace);
      cmsys::auto_ptr<cmCompiledGeneratorExpression> cge = ge.Parse(
                                                                optionGenex);

      this->Internal
        ->CachedLinkInterfaceCompileOptionsEntries[configString].push_back(
                        new cmTargetInternals::TargetPropertyEntry(cge,
                                                              it->Value));
      }
    }

  processCompileOptions(this,
    this->Internal->CachedLinkInterfaceCompileOptionsEntries[configString],
                            result,
                            uniqueOptions,
                            &dagChecker,
                            config,
                            debugOptions);

  if (!this->Makefile->IsGeneratingBuildSystem())
    {
    deleteAndClear(this->Internal->CachedLinkInterfaceCompileOptionsEntries);
    }
  else
    {
    this->Internal->CacheLinkInterfaceCompileOptionsDone[configString] = true;
    }
}

//----------------------------------------------------------------------------
static void processCompileDefinitions(cmTarget const* tgt,
      const std::vector<cmTargetInternals::TargetPropertyEntry*> &entries,
      std::vector<std::string> &options,
      std::set<std::string> &uniqueOptions,
      cmGeneratorExpressionDAGChecker *dagChecker,
      const char *config, bool debugOptions)
{
  processCompileOptionsInternal(tgt, entries, options, uniqueOptions,
                                dagChecker, config, debugOptions,
                                "definitions");
}

//----------------------------------------------------------------------------
void cmTarget::GetCompileDefinitions(std::vector<std::string> &list,
                                            const char *config) const
{
  std::set<std::string> uniqueOptions;
  cmListFileBacktrace lfbt;

  cmGeneratorExpressionDAGChecker dagChecker(lfbt,
                                              this->GetName(),
                                              "COMPILE_DEFINITIONS", 0, 0);

  std::vector<std::string> debugProperties;
  const char *debugProp =
              this->Makefile->GetDefinition("CMAKE_DEBUG_TARGET_PROPERTIES");
  if (debugProp)
    {
    cmSystemTools::ExpandListArgument(debugProp, debugProperties);
    }

  bool debugDefines = !this->DebugCompileDefinitionsDone
                          && std::find(debugProperties.begin(),
                                debugProperties.end(),
                                "COMPILE_DEFINITIONS")
                        != debugProperties.end();

  if (this->Makefile->IsGeneratingBuildSystem())
    {
    this->DebugCompileDefinitionsDone = true;
    }

  processCompileDefinitions(this,
                            this->Internal->CompileDefinitionsEntries,
                            list,
                            uniqueOptions,
                            &dagChecker,
                            config,
                            debugDefines);

  std::string configString = config ? config : "";
  if (!this->Internal->CacheLinkInterfaceCompileDefinitionsDone[configString])
    {
    for (std::vector<cmValueWithOrigin>::const_iterator
        it = this->Internal->LinkImplementationPropertyEntries.begin(),
        end = this->Internal->LinkImplementationPropertyEntries.end();
        it != end; ++it)
      {
      if (!cmGeneratorExpression::IsValidTargetName(it->Value)
          && cmGeneratorExpression::Find(it->Value) == std::string::npos)
        {
        continue;
        }
      {
      cmGeneratorExpression ge(lfbt);
      cmsys::auto_ptr<cmCompiledGeneratorExpression> cge =
                                                        ge.Parse(it->Value);
      std::string targetResult = cge->Evaluate(this->Makefile, config,
                                        false, this, 0, 0);
      if (!this->Makefile->FindTargetToUse(targetResult.c_str()))
        {
        continue;
        }
      }
      std::string defsGenex = "$<TARGET_PROPERTY:" +
                              it->Value + ",INTERFACE_COMPILE_DEFINITIONS>";
      if (cmGeneratorExpression::Find(it->Value) != std::string::npos)
        {
        // Because it->Value is a generator expression, ensure that it
        // evaluates to the non-empty string before being used in the
        // TARGET_PROPERTY expression.
        defsGenex = "$<$<BOOL:" + it->Value + ">:" + defsGenex + ">";
        }
      cmGeneratorExpression ge(it->Backtrace);
      cmsys::auto_ptr<cmCompiledGeneratorExpression> cge = ge.Parse(
                                                                defsGenex);

      this->Internal
        ->CachedLinkInterfaceCompileDefinitionsEntries[configString].push_back(
                        new cmTargetInternals::TargetPropertyEntry(cge,
                                                              it->Value));
      }
    if (config)
      {
      std::string configPropName = "COMPILE_DEFINITIONS_"
                                          + cmSystemTools::UpperCase(config);
      const char *configProp = this->GetProperty(configPropName.c_str());
      if (configProp)
        {
        switch(this->Makefile->GetPolicyStatus(cmPolicies::CMP0043))
          {
          case cmPolicies::WARN:
            {
            cmOStringStream e;
            e << this->Makefile->GetCMakeInstance()->GetPolicies()
                     ->GetPolicyWarning(cmPolicies::CMP0043);
            this->Makefile->IssueMessage(cmake::AUTHOR_WARNING,
                                         e.str().c_str());
            }
          case cmPolicies::OLD:
            {
            cmGeneratorExpression ge(lfbt);
            cmsys::auto_ptr<cmCompiledGeneratorExpression> cge =
                                                        ge.Parse(configProp);
            this->Internal
              ->CachedLinkInterfaceCompileDefinitionsEntries[configString]
                  .push_back(new cmTargetInternals::TargetPropertyEntry(cge));
            }
            break;
          case cmPolicies::NEW:
          case cmPolicies::REQUIRED_ALWAYS:
          case cmPolicies::REQUIRED_IF_USED:
            break;
          }
        }
      }

    }

  processCompileDefinitions(this,
    this->Internal->CachedLinkInterfaceCompileDefinitionsEntries[configString],
                            list,
                            uniqueOptions,
                            &dagChecker,
                            config,
                            debugDefines);

  if (!this->Makefile->IsGeneratingBuildSystem())
    {
    deleteAndClear(this->Internal
                              ->CachedLinkInterfaceCompileDefinitionsEntries);
    }
  else
    {
    this->Internal->CacheLinkInterfaceCompileDefinitionsDone[configString]
                                                                      = true;
    }
}

//----------------------------------------------------------------------------
void cmTarget::MaybeInvalidatePropertyCache(const char* prop)
{
  // Wipe out maps caching information affected by this property.
  if(this->IsImported() && cmHasLiteralPrefix(prop, "IMPORTED"))
    {
    this->Internal->ImportInfoMap.clear();
    }
  if(!this->IsImported() && cmHasLiteralPrefix(prop, "LINK_INTERFACE_"))
    {
    this->ClearLinkMaps();
    }
}

//----------------------------------------------------------------------------
static void cmTargetCheckLINK_INTERFACE_LIBRARIES(
  const char* prop, const char* value, cmMakefile* context, bool imported
  )
{
  // Look for link-type keywords in the value.
  static cmsys::RegularExpression
    keys("(^|;)(debug|optimized|general)(;|$)");
  if(!keys.find(value))
    {
    return;
    }

  // Support imported and non-imported versions of the property.
  const char* base = (imported?
                      "IMPORTED_LINK_INTERFACE_LIBRARIES" :
                      "LINK_INTERFACE_LIBRARIES");

  // Report an error.
  cmOStringStream e;
  e << "Property " << prop << " may not contain link-type keyword \""
    << keys.match(2) << "\".  "
    << "The " << base << " property has a per-configuration "
    << "version called " << base << "_<CONFIG> which may be "
    << "used to specify per-configuration rules.";
  if(!imported)
    {
    e << "  "
      << "Alternatively, an IMPORTED library may be created, configured "
      << "with a per-configuration location, and then named in the "
      << "property value.  "
      << "See the add_library command's IMPORTED mode for details."
      << "\n"
      << "If you have a list of libraries that already contains the "
      << "keyword, use the target_link_libraries command with its "
      << "LINK_INTERFACE_LIBRARIES mode to set the property.  "
      << "The command automatically recognizes link-type keywords and sets "
      << "the LINK_INTERFACE_LIBRARIES and LINK_INTERFACE_LIBRARIES_DEBUG "
      << "properties accordingly.";
    }
  context->IssueMessage(cmake::FATAL_ERROR, e.str());
}

//----------------------------------------------------------------------------
static void cmTargetCheckINTERFACE_LINK_LIBRARIES(const char* value,
                                                  cmMakefile* context)
{
  // Look for link-type keywords in the value.
  static cmsys::RegularExpression
    keys("(^|;)(debug|optimized|general)(;|$)");
  if(!keys.find(value))
    {
    return;
    }

  // Report an error.
  cmOStringStream e;

  e << "Property INTERFACE_LINK_LIBRARIES may not contain link-type "
    "keyword \"" << keys.match(2) << "\".  The INTERFACE_LINK_LIBRARIES "
    "property may contain configuration-sensitive generator-expressions "
    "which may be used to specify per-configuration rules.";

  context->IssueMessage(cmake::FATAL_ERROR, e.str());
}

//----------------------------------------------------------------------------
void cmTarget::CheckProperty(const char* prop, cmMakefile* context) const
{
  // Certain properties need checking.
  if(cmHasLiteralPrefix(prop, "LINK_INTERFACE_LIBRARIES"))
    {
    if(const char* value = this->GetProperty(prop))
      {
      cmTargetCheckLINK_INTERFACE_LIBRARIES(prop, value, context, false);
      }
    }
  if(cmHasLiteralPrefix(prop, "IMPORTED_LINK_INTERFACE_LIBRARIES"))
    {
    if(const char* value = this->GetProperty(prop))
      {
      cmTargetCheckLINK_INTERFACE_LIBRARIES(prop, value, context, true);
      }
    }
  if(cmHasLiteralPrefix(prop, "INTERFACE_LINK_LIBRARIES"))
    {
    if(const char* value = this->GetProperty(prop))
      {
      cmTargetCheckINTERFACE_LINK_LIBRARIES(value, context);
      }
    }
}

//----------------------------------------------------------------------------
void cmTarget::MarkAsImported()
{
  this->IsImportedTarget = true;
}

//----------------------------------------------------------------------------
bool cmTarget::HaveWellDefinedOutputFiles() const
{
  return
    this->GetType() == cmTarget::STATIC_LIBRARY ||
    this->GetType() == cmTarget::SHARED_LIBRARY ||
    this->GetType() == cmTarget::MODULE_LIBRARY ||
    this->GetType() == cmTarget::EXECUTABLE;
}

//----------------------------------------------------------------------------
cmTarget::OutputInfo const* cmTarget::GetOutputInfo(const char* config) const
{
  // There is no output information for imported targets.
  if(this->IsImported())
    {
    return 0;
    }

  // Only libraries and executables have well-defined output files.
  if(!this->HaveWellDefinedOutputFiles())
    {
    std::string msg = "cmTarget::GetOutputInfo called for ";
    msg += this->GetName();
    msg += " which has type ";
    msg += cmTarget::GetTargetTypeName(this->GetType());
    this->GetMakefile()->IssueMessage(cmake::INTERNAL_ERROR, msg);
    abort();
    return 0;
    }

  // Lookup/compute/cache the output information for this configuration.
  std::string config_upper;
  if(config && *config)
    {
    config_upper = cmSystemTools::UpperCase(config);
    }
  typedef cmTargetInternals::OutputInfoMapType OutputInfoMapType;
  OutputInfoMapType::const_iterator i =
    this->Internal->OutputInfoMap.find(config_upper);
  if(i == this->Internal->OutputInfoMap.end())
    {
    OutputInfo info;
    this->ComputeOutputDir(config, false, info.OutDir);
    this->ComputeOutputDir(config, true, info.ImpDir);
    if(!this->ComputePDBOutputDir(config, info.PdbDir))
      {
      info.PdbDir = info.OutDir;
      }
    OutputInfoMapType::value_type entry(config_upper, info);
    i = this->Internal->OutputInfoMap.insert(entry).first;
    }
  return &i->second;
}

//----------------------------------------------------------------------------
std::string cmTarget::GetDirectory(const char* config, bool implib) const
{
  if (this->IsImported())
    {
    // Return the directory from which the target is imported.
    return
      cmSystemTools::GetFilenamePath(
      this->ImportedGetFullPath(config, implib));
    }
  else if(OutputInfo const* info = this->GetOutputInfo(config))
    {
    // Return the directory in which the target will be built.
    return implib? info->ImpDir : info->OutDir;
    }
  return "";
}

//----------------------------------------------------------------------------
std::string cmTarget::GetPDBDirectory(const char* config) const
{
  if(OutputInfo const* info = this->GetOutputInfo(config))
    {
    // Return the directory in which the target will be built.
    return info->PdbDir;
    }
  return "";
}

//----------------------------------------------------------------------------
const char* cmTarget::ImportedGetLocation(const char* config) const
{
  static std::string location;
  location = this->ImportedGetFullPath(config, false);
  return location.c_str();
}

//----------------------------------------------------------------------------
void cmTarget::GetTargetVersion(int& major, int& minor) const
{
  int patch;
  this->GetTargetVersion(false, major, minor, patch);
}

//----------------------------------------------------------------------------
void cmTarget::GetTargetVersion(bool soversion,
                                int& major, int& minor, int& patch) const
{
  // Set the default values.
  major = 0;
  minor = 0;
  patch = 0;

  assert(this->GetType() != INTERFACE_LIBRARY);

  // Look for a VERSION or SOVERSION property.
  const char* prop = soversion? "SOVERSION" : "VERSION";
  if(const char* version = this->GetProperty(prop))
    {
    // Try to parse the version number and store the results that were
    // successfully parsed.
    int parsed_major;
    int parsed_minor;
    int parsed_patch;
    switch(sscanf(version, "%d.%d.%d",
                  &parsed_major, &parsed_minor, &parsed_patch))
      {
      case 3: patch = parsed_patch; // no break!
      case 2: minor = parsed_minor; // no break!
      case 1: major = parsed_major; // no break!
      default: break;
      }
    }
}

//----------------------------------------------------------------------------
const char* cmTarget::GetFeature(const char* feature, const char* config) const
{
  if(config && *config)
    {
    std::string featureConfig = feature;
    featureConfig += "_";
    featureConfig += cmSystemTools::UpperCase(config);
    if(const char* value = this->GetProperty(featureConfig.c_str()))
      {
      return value;
      }
    }
  if(const char* value = this->GetProperty(feature))
    {
    return value;
    }
  return this->Makefile->GetFeature(feature, config);
}

//----------------------------------------------------------------------------
const char *cmTarget::GetProperty(const char* prop) const
{
  return this->GetProperty(prop, cmProperty::TARGET);
}

//----------------------------------------------------------------------------
bool cmTarget::HandleLocationPropertyPolicy() const
{
  if (this->IsImported())
    {
    return true;
    }

  cmOStringStream e;
  e << (this->Makefile->GetPolicies()
        ->GetPolicyWarning(cmPolicies::CMP0026)) << "\n";
  e << "The LOCATION property may not be read from target \""
    << this->GetName() << "\".  Use the target name directly with "
    "add_custom_command, or use the generator expression $<TARGET_FILE>, "
    "as appropriate.\n";
  this->Makefile->IssueMessage(cmake::FATAL_ERROR, e.str().c_str());

  return false;
}

//----------------------------------------------------------------------------
const char *cmTarget::GetProperty(const char* prop,
                                  cmProperty::ScopeType scope) const
{
  if(!prop)
    {
    return 0;
    }

  if (this->GetType() == INTERFACE_LIBRARY
      && !whiteListedInterfaceProperty(prop))
    {
    cmOStringStream e;
    e << "INTERFACE_LIBRARY targets may only have whitelisted properties.  "
         "The property \"" << prop << "\" is not allowed.";
    this->Makefile->IssueMessage(cmake::FATAL_ERROR, e.str().c_str());
    return 0;
    }

  if (strcmp(prop, "NAME") == 0)
    {
    return this->GetName();
    }

  // Watch for special "computed" properties that are dependent on
  // other properties or variables.  Always recompute them.
  if(this->GetType() == cmTarget::EXECUTABLE ||
     this->GetType() == cmTarget::STATIC_LIBRARY ||
     this->GetType() == cmTarget::SHARED_LIBRARY ||
     this->GetType() == cmTarget::MODULE_LIBRARY ||
     this->GetType() == cmTarget::UNKNOWN_LIBRARY)
    {
    if(strcmp(prop,"LOCATION") == 0)
      {
      if (!this->HandleLocationPropertyPolicy())
        {
        return 0;
        }

      // Set the LOCATION property of the target.
      //
      // For an imported target this is the location of an arbitrary
      // available configuration.
      //
      // For a non-imported target this is deprecated because it
      // cannot take into account the per-configuration name of the
      // target because the configuration type may not be known at
      // CMake time.
      this->Properties.SetProperty("LOCATION", this->ImportedGetLocation(0),
                                   cmProperty::TARGET);
      }

    // Support "LOCATION_<CONFIG>".
    if(cmHasLiteralPrefix(prop, "LOCATION_"))
      {
      if (!this->HandleLocationPropertyPolicy())
        {
        return 0;
        }
      std::string configName = prop+9;
      this->Properties.SetProperty(prop,
                                this->ImportedGetLocation(configName.c_str()),
                                cmProperty::TARGET);
      }
    }
  if(strcmp(prop,"INCLUDE_DIRECTORIES") == 0)
    {
    static std::string output;
    output = "";
    std::string sep;
    typedef cmTargetInternals::TargetPropertyEntry
                                TargetPropertyEntry;
    for (std::vector<TargetPropertyEntry*>::const_iterator
        it = this->Internal->IncludeDirectoriesEntries.begin(),
        end = this->Internal->IncludeDirectoriesEntries.end();
        it != end; ++it)
      {
      output += sep;
      output += (*it)->ge->GetInput();
      sep = ";";
      }
    return output.c_str();
    }
  if(strcmp(prop,"COMPILE_OPTIONS") == 0)
    {
    static std::string output;
    output = "";
    std::string sep;
    typedef cmTargetInternals::TargetPropertyEntry
                                TargetPropertyEntry;
    for (std::vector<TargetPropertyEntry*>::const_iterator
        it = this->Internal->CompileOptionsEntries.begin(),
        end = this->Internal->CompileOptionsEntries.end();
        it != end; ++it)
      {
      output += sep;
      output += (*it)->ge->GetInput();
      sep = ";";
      }
    return output.c_str();
    }
  if(strcmp(prop,"COMPILE_DEFINITIONS") == 0)
    {
    static std::string output;
    output = "";
    std::string sep;
    typedef cmTargetInternals::TargetPropertyEntry
                                TargetPropertyEntry;
    for (std::vector<TargetPropertyEntry*>::const_iterator
        it = this->Internal->CompileDefinitionsEntries.begin(),
        end = this->Internal->CompileDefinitionsEntries.end();
        it != end; ++it)
      {
      output += sep;
      output += (*it)->ge->GetInput();
      sep = ";";
      }
    return output.c_str();
    }
  if(strcmp(prop,"LINK_LIBRARIES") == 0)
    {
    static std::string output;
    output = "";
    std::string sep;
    for (std::vector<cmValueWithOrigin>::const_iterator
        it = this->Internal->LinkImplementationPropertyEntries.begin(),
        end = this->Internal->LinkImplementationPropertyEntries.end();
        it != end; ++it)
      {
      output += sep;
      output += it->Value;
      sep = ";";
      }
    return output.c_str();
    }

  if (strcmp(prop,"IMPORTED") == 0)
    {
    return this->IsImported()?"TRUE":"FALSE";
    }

  if(!strcmp(prop,"SOURCES"))
    {
    cmOStringStream ss;
    const char* sep = "";
    for(std::vector<cmSourceFile*>::const_iterator
          i = this->SourceFiles.begin();
        i != this->SourceFiles.end(); ++i)
      {
      // Separate from the previous list entries.
      ss << sep;
      sep = ";";

      // Construct what is known about this source file location.
      cmSourceFileLocation const& location = (*i)->GetLocation();
      std::string sname = location.GetDirectory();
      if(!sname.empty())
        {
        sname += "/";
        }
      sname += location.GetName();

      // Append this list entry.
      ss << sname;
      }
    this->Properties.SetProperty("SOURCES", ss.str().c_str(),
                                 cmProperty::TARGET);
    }

  // the type property returns what type the target is
  if (!strcmp(prop,"TYPE"))
    {
    return cmTarget::GetTargetTypeName(this->GetType());
    }
  bool chain = false;
  const char *retVal =
    this->Properties.GetPropertyValue(prop, scope, chain);
  if (chain)
    {
    return this->Makefile->GetProperty(prop,scope);
    }
  return retVal;
}

//----------------------------------------------------------------------------
bool cmTarget::GetPropertyAsBool(const char* prop) const
{
  return cmSystemTools::IsOn(this->GetProperty(prop));
}
//----------------------------------------------------------------------------
const char* cmTarget::GetSuffixVariableInternal(bool implib) const
{
  switch(this->GetType())
    {
    case cmTarget::STATIC_LIBRARY:
      return "CMAKE_STATIC_LIBRARY_SUFFIX";
    case cmTarget::SHARED_LIBRARY:
      return (implib
              ? "CMAKE_IMPORT_LIBRARY_SUFFIX"
              : "CMAKE_SHARED_LIBRARY_SUFFIX");
    case cmTarget::MODULE_LIBRARY:
      return (implib
              ? "CMAKE_IMPORT_LIBRARY_SUFFIX"
              : "CMAKE_SHARED_MODULE_SUFFIX");
    case cmTarget::EXECUTABLE:
      return (implib
              ? "CMAKE_IMPORT_LIBRARY_SUFFIX"
              : "CMAKE_EXECUTABLE_SUFFIX");
    default:
      break;
    }
  return "";
}


//----------------------------------------------------------------------------
const char* cmTarget::GetPrefixVariableInternal(bool implib) const
{
  switch(this->GetType())
    {
    case cmTarget::STATIC_LIBRARY:
      return "CMAKE_STATIC_LIBRARY_PREFIX";
    case cmTarget::SHARED_LIBRARY:
      return (implib
              ? "CMAKE_IMPORT_LIBRARY_PREFIX"
              : "CMAKE_SHARED_LIBRARY_PREFIX");
    case cmTarget::MODULE_LIBRARY:
      return (implib
              ? "CMAKE_IMPORT_LIBRARY_PREFIX"
              : "CMAKE_SHARED_MODULE_PREFIX");
    case cmTarget::EXECUTABLE:
      return (implib? "CMAKE_IMPORT_LIBRARY_PREFIX" : "");
    default:
      break;
    }
  return "";
}

//----------------------------------------------------------------------------
bool cmTarget::HasMacOSXRpathInstallNameDir(const char* config) const
{
  bool install_name_is_rpath = false;
  bool macosx_rpath = false;

  if(!this->IsImportedTarget)
    {
    if(this->GetType() != cmTarget::SHARED_LIBRARY)
      {
      return false;
      }
    const char* install_name = this->GetProperty("INSTALL_NAME_DIR");
    bool use_install_name =
      this->GetPropertyAsBool("BUILD_WITH_INSTALL_RPATH");
    if(install_name && use_install_name &&
       std::string(install_name) == "@rpath")
      {
      install_name_is_rpath = true;
      }
    else if(install_name && use_install_name)
      {
      return false;
      }
    if(!install_name_is_rpath)
      {
      macosx_rpath = this->MacOSXRpathInstallNameDirDefault();
      }
    }
  else
    {
    // Lookup the imported soname.
    if(cmTarget::ImportInfo const* info = this->GetImportInfo(config, this))
      {
      if(!info->NoSOName && !info->SOName.empty())
        {
        if(info->SOName.find("@rpath/") == 0)
          {
          install_name_is_rpath = true;
          }
        }
      else
        {
        std::string install_name;
        cmSystemTools::GuessLibraryInstallName(info->Location, install_name);
        if(install_name.find("@rpath") != std::string::npos)
          {
          install_name_is_rpath = true;
          }
        }
      }
    }

  if(!install_name_is_rpath && !macosx_rpath)
    {
    return false;
    }

  if(!this->Makefile->IsSet("CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG"))
    {
    cmOStringStream w;
    w << "Attempting to use";
    if(macosx_rpath)
      {
      w << " MACOSX_RPATH";
      }
    else
      {
      w << " @rpath";
      }
    w << " without CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG being set.";
    w << "  This could be because you are using a Mac OS X version";
    w << " less than 10.5 or because CMake's platform configuration is";
    w << " corrupt.";
    cmake* cm = this->Makefile->GetCMakeInstance();
    cm->IssueMessage(cmake::FATAL_ERROR, w.str(), this->GetBacktrace());
    }

  return true;
}

//----------------------------------------------------------------------------
bool cmTarget::MacOSXRpathInstallNameDirDefault() const
{
  // we can't do rpaths when unsupported
  if(!this->Makefile->IsSet("CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG"))
    {
    return false;
    }

  const char* macosx_rpath_str = this->GetProperty("MACOSX_RPATH");
  if(macosx_rpath_str)
    {
    return this->GetPropertyAsBool("MACOSX_RPATH");
    }

  cmPolicies::PolicyStatus cmp0042 = this->GetPolicyStatusCMP0042();

  if(cmp0042 == cmPolicies::WARN)
    {
    this->Makefile->GetLocalGenerator()->GetGlobalGenerator()->
      AddCMP0042WarnTarget(this->GetName());
    }

  if(cmp0042 == cmPolicies::NEW)
    {
    return true;
    }

  return false;
}

//----------------------------------------------------------------------------
bool cmTarget::IsImportedSharedLibWithoutSOName(const char* config) const
{
  if(this->IsImported() && this->GetType() == cmTarget::SHARED_LIBRARY)
    {
    if(cmTarget::ImportInfo const* info = this->GetImportInfo(config, this))
      {
      return info->NoSOName;
      }
    }
  return false;
}

//----------------------------------------------------------------------------
std::string
cmTarget::GetFullNameImported(const char* config, bool implib) const
{
  return cmSystemTools::GetFilenameName(
    this->ImportedGetFullPath(config, implib));
}

//----------------------------------------------------------------------------
std::string
cmTarget::ImportedGetFullPath(const char* config, bool implib) const
{
  std::string result;
  if(cmTarget::ImportInfo const* info = this->GetImportInfo(config, this))
    {
    result = implib? info->ImportLibrary : info->Location;
    }
  if(result.empty())
    {
    result = this->GetName();
    result += "-NOTFOUND";
    }
  return result;
}

//----------------------------------------------------------------------------
void cmTarget::ComputeVersionedName(std::string& vName,
                                    std::string const& prefix,
                                    std::string const& base,
                                    std::string const& suffix,
                                    std::string const& name,
                                    const char* version) const
{
  vName = this->IsApple? (prefix+base) : name;
  if(version)
    {
    vName += ".";
    vName += version;
    }
  vName += this->IsApple? suffix : std::string();
}

//----------------------------------------------------------------------------
bool cmTarget::HasImplibGNUtoMS() const
{
  return this->HasImportLibrary() && this->GetPropertyAsBool("GNUtoMS");
}

//----------------------------------------------------------------------------
bool cmTarget::GetImplibGNUtoMS(std::string const& gnuName,
                                std::string& out, const char* newExt) const
{
  if(this->HasImplibGNUtoMS() &&
     gnuName.size() > 6 && gnuName.substr(gnuName.size()-6) == ".dll.a")
    {
    out = gnuName.substr(0, gnuName.size()-6);
    out += newExt? newExt : ".lib";
    return true;
    }
  return false;
}

//----------------------------------------------------------------------------
void cmTarget::SetPropertyDefault(const char* property,
                                  const char* default_value)
{
  // Compute the name of the variable holding the default value.
  std::string var = "CMAKE_";
  var += property;

  if(const char* value = this->Makefile->GetDefinition(var.c_str()))
    {
    this->SetProperty(property, value);
    }
  else if(default_value)
    {
    this->SetProperty(property, default_value);
    }
}

//----------------------------------------------------------------------------
bool cmTarget::HaveBuildTreeRPATH(const char *config) const
{
  if (this->GetPropertyAsBool("SKIP_BUILD_RPATH"))
    {
    return false;
    }
  std::vector<std::string> libs;
  this->GetDirectLinkLibraries(config, libs, this);
  return !libs.empty();
}

//----------------------------------------------------------------------------
bool cmTarget::HaveInstallTreeRPATH() const
{
  const char* install_rpath = this->GetProperty("INSTALL_RPATH");
  return (install_rpath && *install_rpath) &&
          !this->Makefile->IsOn("CMAKE_SKIP_INSTALL_RPATH");
}

//----------------------------------------------------------------------------
const char* cmTarget::GetOutputTargetType(bool implib) const
{
  switch(this->GetType())
    {
    case cmTarget::SHARED_LIBRARY:
      if(this->DLLPlatform)
        {
        if(implib)
          {
          // A DLL import library is treated as an archive target.
          return "ARCHIVE";
          }
        else
          {
          // A DLL shared library is treated as a runtime target.
          return "RUNTIME";
          }
        }
      else
        {
        // For non-DLL platforms shared libraries are treated as
        // library targets.
        return "LIBRARY";
        }
    case cmTarget::STATIC_LIBRARY:
      // Static libraries are always treated as archive targets.
      return "ARCHIVE";
    case cmTarget::MODULE_LIBRARY:
      if(implib)
        {
        // Module libraries are always treated as library targets.
        return "ARCHIVE";
        }
      else
        {
        // Module import libraries are treated as archive targets.
        return "LIBRARY";
        }
    case cmTarget::EXECUTABLE:
      if(implib)
        {
        // Executable import libraries are treated as archive targets.
        return "ARCHIVE";
        }
      else
        {
        // Executables are always treated as runtime targets.
        return "RUNTIME";
        }
    default:
      break;
    }
  return "";
}

//----------------------------------------------------------------------------
bool cmTarget::ComputeOutputDir(const char* config,
                                bool implib, std::string& out) const
{
  bool usesDefaultOutputDir = false;

  // Look for a target property defining the target output directory
  // based on the target type.
  std::string targetTypeName = this->GetOutputTargetType(implib);
  const char* propertyName = 0;
  std::string propertyNameStr = targetTypeName;
  if(!propertyNameStr.empty())
    {
    propertyNameStr += "_OUTPUT_DIRECTORY";
    propertyName = propertyNameStr.c_str();
    }

  // Check for a per-configuration output directory target property.
  std::string configUpper = cmSystemTools::UpperCase(config? config : "");
  const char* configProp = 0;
  std::string configPropStr = targetTypeName;
  if(!configPropStr.empty())
    {
    configPropStr += "_OUTPUT_DIRECTORY_";
    configPropStr += configUpper;
    configProp = configPropStr.c_str();
    }

  // Select an output directory.
  if(const char* config_outdir = this->GetProperty(configProp))
    {
    // Use the user-specified per-configuration output directory.
    out = config_outdir;

    // Skip per-configuration subdirectory.
    config = 0;
    }
  else if(const char* outdir = this->GetProperty(propertyName))
    {
    // Use the user-specified output directory.
    out = outdir;
    }
  else if(this->GetType() == cmTarget::EXECUTABLE)
    {
    // Lookup the output path for executables.
    out = this->Makefile->GetSafeDefinition("EXECUTABLE_OUTPUT_PATH");
    }
  else if(this->GetType() == cmTarget::STATIC_LIBRARY ||
          this->GetType() == cmTarget::SHARED_LIBRARY ||
          this->GetType() == cmTarget::MODULE_LIBRARY)
    {
    // Lookup the output path for libraries.
    out = this->Makefile->GetSafeDefinition("LIBRARY_OUTPUT_PATH");
    }
  if(out.empty())
    {
    // Default to the current output directory.
    usesDefaultOutputDir = true;
    out = ".";
    }

  // Convert the output path to a full path in case it is
  // specified as a relative path.  Treat a relative path as
  // relative to the current output directory for this makefile.
  out = (cmSystemTools::CollapseFullPath
         (out.c_str(), this->Makefile->GetStartOutputDirectory()));

  // The generator may add the configuration's subdirectory.
  if(config && *config)
    {
    const char *platforms = this->Makefile->GetDefinition(
      "CMAKE_XCODE_EFFECTIVE_PLATFORMS");
    std::string suffix =
      usesDefaultOutputDir && platforms ? "$(EFFECTIVE_PLATFORM_NAME)" : "";
    this->Makefile->GetLocalGenerator()->GetGlobalGenerator()->
      AppendDirectoryForConfig("/", config, suffix.c_str(), out);
    }

  return usesDefaultOutputDir;
}

//----------------------------------------------------------------------------
bool cmTarget::ComputePDBOutputDir(const char* config, std::string& out) const
{
  // Look for a target property defining the target output directory
  // based on the target type.
  std::string targetTypeName = "PDB";
  const char* propertyName = 0;
  std::string propertyNameStr = targetTypeName;
  if(!propertyNameStr.empty())
    {
    propertyNameStr += "_OUTPUT_DIRECTORY";
    propertyName = propertyNameStr.c_str();
    }

  // Check for a per-configuration output directory target property.
  std::string configUpper = cmSystemTools::UpperCase(config? config : "");
  const char* configProp = 0;
  std::string configPropStr = targetTypeName;
  if(!configPropStr.empty())
    {
    configPropStr += "_OUTPUT_DIRECTORY_";
    configPropStr += configUpper;
    configProp = configPropStr.c_str();
    }

  // Select an output directory.
  if(const char* config_outdir = this->GetProperty(configProp))
    {
    // Use the user-specified per-configuration output directory.
    out = config_outdir;

    // Skip per-configuration subdirectory.
    config = 0;
    }
  else if(const char* outdir = this->GetProperty(propertyName))
    {
    // Use the user-specified output directory.
    out = outdir;
    }
  if(out.empty())
    {
    return false;
    }

  // Convert the output path to a full path in case it is
  // specified as a relative path.  Treat a relative path as
  // relative to the current output directory for this makefile.
  out = (cmSystemTools::CollapseFullPath
         (out.c_str(), this->Makefile->GetStartOutputDirectory()));

  // The generator may add the configuration's subdirectory.
  if(config && *config)
    {
    this->Makefile->GetLocalGenerator()->GetGlobalGenerator()->
      AppendDirectoryForConfig("/", config, "", out);
    }
  return true;
}

//----------------------------------------------------------------------------
bool cmTarget::UsesDefaultOutputDir(const char* config, bool implib) const
{
  std::string dir;
  return this->ComputeOutputDir(config, implib, dir);
}

//----------------------------------------------------------------------------
std::string cmTarget::GetOutputName(const char* config, bool implib) const
{
  std::vector<std::string> props;
  std::string type = this->GetOutputTargetType(implib);
  std::string configUpper = cmSystemTools::UpperCase(config? config : "");
  if(!type.empty() && !configUpper.empty())
    {
    // <ARCHIVE|LIBRARY|RUNTIME>_OUTPUT_NAME_<CONFIG>
    props.push_back(type + "_OUTPUT_NAME_" + configUpper);
    }
  if(!type.empty())
    {
    // <ARCHIVE|LIBRARY|RUNTIME>_OUTPUT_NAME
    props.push_back(type + "_OUTPUT_NAME");
    }
  if(!configUpper.empty())
    {
    // OUTPUT_NAME_<CONFIG>
    props.push_back("OUTPUT_NAME_" + configUpper);
    // <CONFIG>_OUTPUT_NAME
    props.push_back(configUpper + "_OUTPUT_NAME");
    }
  // OUTPUT_NAME
  props.push_back("OUTPUT_NAME");

  for(std::vector<std::string>::const_iterator i = props.begin();
      i != props.end(); ++i)
    {
    if(const char* outName = this->GetProperty(i->c_str()))
      {
      return outName;
      }
    }
  return this->GetName();
}

//----------------------------------------------------------------------------
std::string cmTarget::GetFrameworkVersion() const
{
  assert(this->GetType() != INTERFACE_LIBRARY);

  if(const char* fversion = this->GetProperty("FRAMEWORK_VERSION"))
    {
    return fversion;
    }
  else if(const char* tversion = this->GetProperty("VERSION"))
    {
    return tversion;
    }
  else
    {
    return "A";
    }
}

//----------------------------------------------------------------------------
const char* cmTarget::GetExportMacro() const
{
  // Define the symbol for targets that export symbols.
  if(this->GetType() == cmTarget::SHARED_LIBRARY ||
     this->GetType() == cmTarget::MODULE_LIBRARY ||
     this->IsExecutableWithExports())
    {
    if(const char* custom_export_name = this->GetProperty("DEFINE_SYMBOL"))
      {
      this->ExportMacro = custom_export_name;
      }
    else
      {
      std::string in = this->GetName();
      in += "_EXPORTS";
      this->ExportMacro = cmSystemTools::MakeCindentifier(in.c_str());
      }
    return this->ExportMacro.c_str();
    }
  else
    {
    return 0;
    }
}

//----------------------------------------------------------------------------
bool cmTarget::IsNullImpliedByLinkLibraries(const std::string &p) const
{
  return this->LinkImplicitNullProperties.find(p)
      != this->LinkImplicitNullProperties.end();
}

//----------------------------------------------------------------------------
void cmTarget::GetLanguages(std::set<cmStdString>& languages) const
{
  for(std::vector<cmSourceFile*>::const_iterator
        i = this->SourceFiles.begin(); i != this->SourceFiles.end(); ++i)
    {
    if(const char* lang = (*i)->GetLanguage())
      {
      languages.insert(lang);
      }
    }
}

//----------------------------------------------------------------------------
cmTarget::ImportInfo const*
cmTarget::GetImportInfo(const char* config, cmTarget const* headTarget) const
{
  // There is no imported information for non-imported targets.
  if(!this->IsImported())
    {
    return 0;
    }

  // Lookup/compute/cache the import information for this
  // configuration.
  std::string config_upper;
  if(config && *config)
    {
    config_upper = cmSystemTools::UpperCase(config);
    }
  else
    {
    config_upper = "NOCONFIG";
    }
  TargetConfigPair key(headTarget, config_upper);
  typedef cmTargetInternals::ImportInfoMapType ImportInfoMapType;

  ImportInfoMapType::const_iterator i =
    this->Internal->ImportInfoMap.find(key);
  if(i == this->Internal->ImportInfoMap.end())
    {
    ImportInfo info;
    this->ComputeImportInfo(config_upper, info, headTarget);
    ImportInfoMapType::value_type entry(key, info);
    i = this->Internal->ImportInfoMap.insert(entry).first;
    }

  if(this->GetType() == INTERFACE_LIBRARY)
    {
    return &i->second;
    }
  // If the location is empty then the target is not available for
  // this configuration.
  if(i->second.Location.empty() && i->second.ImportLibrary.empty())
    {
    return 0;
    }

  // Return the import information.
  return &i->second;
}

bool cmTarget::GetMappedConfig(std::string const& desired_config,
                               const char** loc,
                               const char** imp,
                               std::string& suffix) const
{
  if (this->GetType() == INTERFACE_LIBRARY)
    {
    // This method attempts to find a config-specific LOCATION for the
    // IMPORTED library. In the case of INTERFACE_LIBRARY, there is no
    // LOCATION at all, so leaving *loc and *imp unchanged is the appropriate
    // and valid response.
    return true;
    }

  // Track the configuration-specific property suffix.
  suffix = "_";
  suffix += desired_config;

  std::vector<std::string> mappedConfigs;
  {
  std::string mapProp = "MAP_IMPORTED_CONFIG_";
  mapProp += desired_config;
  if(const char* mapValue = this->GetProperty(mapProp.c_str()))
    {
    cmSystemTools::ExpandListArgument(mapValue, mappedConfigs);
    }
  }

  // If we needed to find one of the mapped configurations but did not
  // On a DLL platform there may be only IMPORTED_IMPLIB for a shared
  // library or an executable with exports.
  bool allowImp = this->HasImportLibrary();

  // If a mapping was found, check its configurations.
  for(std::vector<std::string>::const_iterator mci = mappedConfigs.begin();
      !*loc && !*imp && mci != mappedConfigs.end(); ++mci)
    {
    // Look for this configuration.
    std::string mcUpper = cmSystemTools::UpperCase(mci->c_str());
    std::string locProp = "IMPORTED_LOCATION_";
    locProp += mcUpper;
    *loc = this->GetProperty(locProp.c_str());
    if(allowImp)
      {
      std::string impProp = "IMPORTED_IMPLIB_";
      impProp += mcUpper;
      *imp = this->GetProperty(impProp.c_str());
      }

    // If it was found, use it for all properties below.
    if(*loc || *imp)
      {
      suffix = "_";
      suffix += mcUpper;
      }
    }

  // If we needed to find one of the mapped configurations but did not
  // then the target is not found.  The project does not want any
  // other configuration.
  if(!mappedConfigs.empty() && !*loc && !*imp)
    {
    return false;
    }

  // If we have not yet found it then there are no mapped
  // configurations.  Look for an exact-match.
  if(!*loc && !*imp)
    {
    std::string locProp = "IMPORTED_LOCATION";
    locProp += suffix;
    *loc = this->GetProperty(locProp.c_str());
    if(allowImp)
      {
      std::string impProp = "IMPORTED_IMPLIB";
      impProp += suffix;
      *imp = this->GetProperty(impProp.c_str());
      }
    }

  // If we have not yet found it then there are no mapped
  // configurations and no exact match.
  if(!*loc && !*imp)
    {
    // The suffix computed above is not useful.
    suffix = "";

    // Look for a configuration-less location.  This may be set by
    // manually-written code.
    *loc = this->GetProperty("IMPORTED_LOCATION");
    if(allowImp)
      {
      *imp = this->GetProperty("IMPORTED_IMPLIB");
      }
    }

  // If we have not yet found it then the project is willing to try
  // any available configuration.
  if(!*loc && !*imp)
    {
    std::vector<std::string> availableConfigs;
    if(const char* iconfigs = this->GetProperty("IMPORTED_CONFIGURATIONS"))
      {
      cmSystemTools::ExpandListArgument(iconfigs, availableConfigs);
      }
    for(std::vector<std::string>::const_iterator
          aci = availableConfigs.begin();
        !*loc && !*imp && aci != availableConfigs.end(); ++aci)
      {
      suffix = "_";
      suffix += cmSystemTools::UpperCase(*aci);
      std::string locProp = "IMPORTED_LOCATION";
      locProp += suffix;
      *loc = this->GetProperty(locProp.c_str());
      if(allowImp)
        {
        std::string impProp = "IMPORTED_IMPLIB";
        impProp += suffix;
        *imp = this->GetProperty(impProp.c_str());
        }
      }
    }
  // If we have not yet found it then the target is not available.
  if(!*loc && !*imp)
    {
    return false;
    }

  return true;
}

//----------------------------------------------------------------------------
void cmTarget::ComputeImportInfo(std::string const& desired_config,
                                 ImportInfo& info,
                                 cmTarget const* headTarget) const
{
  // This method finds information about an imported target from its
  // properties.  The "IMPORTED_" namespace is reserved for properties
  // defined by the project exporting the target.

  // Initialize members.
  info.NoSOName = false;

  const char* loc = 0;
  const char* imp = 0;
  std::string suffix;
  if (!this->GetMappedConfig(desired_config, &loc, &imp, suffix))
    {
    return;
    }

  // Get the link interface.
  {
  std::string linkProp = "INTERFACE_LINK_LIBRARIES";
  const char *propertyLibs = this->GetProperty(linkProp.c_str());

  if (this->GetType() != INTERFACE_LIBRARY)
    {
    if(!propertyLibs)
      {
      linkProp = "IMPORTED_LINK_INTERFACE_LIBRARIES";
      linkProp += suffix;
      propertyLibs = this->GetProperty(linkProp.c_str());
      }

    if(!propertyLibs)
      {
      linkProp = "IMPORTED_LINK_INTERFACE_LIBRARIES";
      propertyLibs = this->GetProperty(linkProp.c_str());
      }
    }
  if(propertyLibs)
    {
    cmListFileBacktrace lfbt;
    cmGeneratorExpression ge(lfbt);

    cmGeneratorExpressionDAGChecker dagChecker(lfbt,
                                        this->GetName(),
                                        linkProp, 0, 0);
    cmSystemTools::ExpandListArgument(ge.Parse(propertyLibs)
                                       ->Evaluate(this->Makefile,
                                                  desired_config.c_str(),
                                                  false,
                                                  headTarget,
                                                  this,
                                                  &dagChecker),
                                    info.LinkInterface.Libraries);
    }
  }
  if(this->GetType() == INTERFACE_LIBRARY)
    {
    return;
    }

  // A provided configuration has been chosen.  Load the
  // configuration's properties.

  // Get the location.
  if(loc)
    {
    info.Location = loc;
    }
  else
    {
    std::string impProp = "IMPORTED_LOCATION";
    impProp += suffix;
    if(const char* config_location = this->GetProperty(impProp.c_str()))
      {
      info.Location = config_location;
      }
    else if(const char* location = this->GetProperty("IMPORTED_LOCATION"))
      {
      info.Location = location;
      }
    }

  // Get the soname.
  if(this->GetType() == cmTarget::SHARED_LIBRARY)
    {
    std::string soProp = "IMPORTED_SONAME";
    soProp += suffix;
    if(const char* config_soname = this->GetProperty(soProp.c_str()))
      {
      info.SOName = config_soname;
      }
    else if(const char* soname = this->GetProperty("IMPORTED_SONAME"))
      {
      info.SOName = soname;
      }
    }

  // Get the "no-soname" mark.
  if(this->GetType() == cmTarget::SHARED_LIBRARY)
    {
    std::string soProp = "IMPORTED_NO_SONAME";
    soProp += suffix;
    if(const char* config_no_soname = this->GetProperty(soProp.c_str()))
      {
      info.NoSOName = cmSystemTools::IsOn(config_no_soname);
      }
    else if(const char* no_soname = this->GetProperty("IMPORTED_NO_SONAME"))
      {
      info.NoSOName = cmSystemTools::IsOn(no_soname);
      }
    }

  // Get the import library.
  if(imp)
    {
    info.ImportLibrary = imp;
    }
  else if(this->GetType() == cmTarget::SHARED_LIBRARY ||
          this->IsExecutableWithExports())
    {
    std::string impProp = "IMPORTED_IMPLIB";
    impProp += suffix;
    if(const char* config_implib = this->GetProperty(impProp.c_str()))
      {
      info.ImportLibrary = config_implib;
      }
    else if(const char* implib = this->GetProperty("IMPORTED_IMPLIB"))
      {
      info.ImportLibrary = implib;
      }
    }

  // Get the link dependencies.
  {
  std::string linkProp = "IMPORTED_LINK_DEPENDENT_LIBRARIES";
  linkProp += suffix;
  if(const char* config_libs = this->GetProperty(linkProp.c_str()))
    {
    cmSystemTools::ExpandListArgument(config_libs,
                                      info.LinkInterface.SharedDeps);
    }
  else if(const char* libs =
          this->GetProperty("IMPORTED_LINK_DEPENDENT_LIBRARIES"))
    {
    cmSystemTools::ExpandListArgument(libs, info.LinkInterface.SharedDeps);
    }
  }

  // Get the link languages.
  if(this->LinkLanguagePropagatesToDependents())
    {
    std::string linkProp = "IMPORTED_LINK_INTERFACE_LANGUAGES";
    linkProp += suffix;
    if(const char* config_libs = this->GetProperty(linkProp.c_str()))
      {
      cmSystemTools::ExpandListArgument(config_libs,
                                        info.LinkInterface.Languages);
      }
    else if(const char* libs =
            this->GetProperty("IMPORTED_LINK_INTERFACE_LANGUAGES"))
      {
      cmSystemTools::ExpandListArgument(libs,
                                        info.LinkInterface.Languages);
      }
    }

  // Get the cyclic repetition count.
  if(this->GetType() == cmTarget::STATIC_LIBRARY)
    {
    std::string linkProp = "IMPORTED_LINK_INTERFACE_MULTIPLICITY";
    linkProp += suffix;
    if(const char* config_reps = this->GetProperty(linkProp.c_str()))
      {
      sscanf(config_reps, "%u", &info.LinkInterface.Multiplicity);
      }
    else if(const char* reps =
            this->GetProperty("IMPORTED_LINK_INTERFACE_MULTIPLICITY"))
      {
      sscanf(reps, "%u", &info.LinkInterface.Multiplicity);
      }
    }
}

//----------------------------------------------------------------------------
cmTarget::LinkInterface const* cmTarget::GetLinkInterface(const char* config,
                                                  cmTarget const* head) const
{
  // Imported targets have their own link interface.
  if(this->IsImported())
    {
    if(cmTarget::ImportInfo const* info = this->GetImportInfo(config, head))
      {
      return &info->LinkInterface;
      }
    return 0;
    }

  // Link interfaces are not supported for executables that do not
  // export symbols.
  if(this->GetType() == cmTarget::EXECUTABLE &&
     !this->IsExecutableWithExports())
    {
    return 0;
    }

  // Lookup any existing link interface for this configuration.
  TargetConfigPair key(head, cmSystemTools::UpperCase(config? config : ""));

  cmTargetInternals::LinkInterfaceMapType::iterator
    i = this->Internal->LinkInterfaceMap.find(key);
  if(i == this->Internal->LinkInterfaceMap.end())
    {
    // Compute the link interface for this configuration.
    cmTargetInternals::OptionalLinkInterface iface;
    iface.Exists = this->ComputeLinkInterface(config, iface, head);

    // Store the information for this configuration.
    cmTargetInternals::LinkInterfaceMapType::value_type entry(key, iface);
    i = this->Internal->LinkInterfaceMap.insert(entry).first;
    }

  return i->second.Exists? &i->second : 0;
}

//----------------------------------------------------------------------------
void cmTarget::GetTransitivePropertyLinkLibraries(
                                      const char* config,
                                      cmTarget const* headTarget,
                                      std::vector<std::string> &libs) const
{
  cmTarget::LinkInterface const* iface = this->GetLinkInterface(config,
                                                                headTarget);
  if (!iface)
    {
    return;
    }
  if(this->GetType() != STATIC_LIBRARY
      || this->GetPolicyStatusCMP0022() == cmPolicies::WARN
      || this->GetPolicyStatusCMP0022() == cmPolicies::OLD)
    {
    libs = iface->Libraries;
    return;
    }

  const char* linkIfaceProp = "INTERFACE_LINK_LIBRARIES";
  const char* interfaceLibs = this->GetProperty(linkIfaceProp);

  if (!interfaceLibs)
    {
    return;
    }

  // The interface libraries have been explicitly set.
  cmListFileBacktrace lfbt;
  cmGeneratorExpression ge(lfbt);
  cmGeneratorExpressionDAGChecker dagChecker(lfbt, this->GetName(),
                                              linkIfaceProp, 0, 0);
  dagChecker.SetTransitivePropertiesOnly();
  cmSystemTools::ExpandListArgument(ge.Parse(interfaceLibs)->Evaluate(
                                      this->Makefile,
                                      config,
                                      false,
                                      headTarget,
                                      this, &dagChecker), libs);
}

//----------------------------------------------------------------------------
bool cmTarget::ComputeLinkInterface(const char* config, LinkInterface& iface,
                                    cmTarget const* headTarget) const
{
  // Construct the property name suffix for this configuration.
  std::string suffix = "_";
  if(config && *config)
    {
    suffix += cmSystemTools::UpperCase(config);
    }
  else
    {
    suffix += "NOCONFIG";
    }

  // An explicit list of interface libraries may be set for shared
  // libraries and executables that export symbols.
  const char* explicitLibraries = 0;
  std::string linkIfaceProp;
  if(this->PolicyStatusCMP0022 != cmPolicies::OLD &&
     this->PolicyStatusCMP0022 != cmPolicies::WARN)
    {
    // CMP0022 NEW behavior is to use INTERFACE_LINK_LIBRARIES.
    linkIfaceProp = "INTERFACE_LINK_LIBRARIES";
    explicitLibraries = this->GetProperty(linkIfaceProp.c_str());
    }
  else if(this->GetType() == cmTarget::SHARED_LIBRARY ||
          this->IsExecutableWithExports())
    {
    // CMP0022 OLD behavior is to use LINK_INTERFACE_LIBRARIES if set on a
    // shared lib or executable.

    // Lookup the per-configuration property.
    linkIfaceProp = "LINK_INTERFACE_LIBRARIES";
    linkIfaceProp += suffix;
    explicitLibraries = this->GetProperty(linkIfaceProp.c_str());

    // If not set, try the generic property.
    if(!explicitLibraries)
      {
      linkIfaceProp = "LINK_INTERFACE_LIBRARIES";
      explicitLibraries = this->GetProperty(linkIfaceProp.c_str());
      }
    }

  if(explicitLibraries && this->PolicyStatusCMP0022 == cmPolicies::WARN &&
     !this->Internal->PolicyWarnedCMP0022)
    {
    // Compare the explicitly set old link interface properties to the
    // preferred new link interface property one and warn if different.
    const char* newExplicitLibraries =
      this->GetProperty("INTERFACE_LINK_LIBRARIES");
    if (newExplicitLibraries
        && strcmp(newExplicitLibraries, explicitLibraries) != 0)
      {
      cmOStringStream w;
      w <<
        (this->Makefile->GetPolicies()
         ->GetPolicyWarning(cmPolicies::CMP0022)) << "\n"
        "Target \"" << this->GetName() << "\" has an "
        "INTERFACE_LINK_LIBRARIES property which differs from its " <<
        linkIfaceProp << " properties."
        "\n"
        "INTERFACE_LINK_LIBRARIES:\n"
        "  " << newExplicitLibraries << "\n" <<
        linkIfaceProp << ":\n"
        "  " << (explicitLibraries ? explicitLibraries : "(empty)") << "\n";
      this->Makefile->IssueMessage(cmake::AUTHOR_WARNING, w.str());
      this->Internal->PolicyWarnedCMP0022 = true;
      }
    }

  // There is no implicit link interface for executables or modules
  // so if none was explicitly set then there is no link interface.
  if(!explicitLibraries &&
     (this->GetType() == cmTarget::EXECUTABLE ||
      (this->GetType() == cmTarget::MODULE_LIBRARY)))
    {
    return false;
    }

  if(explicitLibraries)
    {
    // The interface libraries have been explicitly set.
    cmListFileBacktrace lfbt;
    cmGeneratorExpression ge(lfbt);
    cmGeneratorExpressionDAGChecker dagChecker(lfbt, this->GetName(),
                                               linkIfaceProp, 0, 0);
    cmSystemTools::ExpandListArgument(ge.Parse(explicitLibraries)->Evaluate(
                                        this->Makefile,
                                        config,
                                        false,
                                        headTarget,
                                        this, &dagChecker), iface.Libraries);

    if(this->GetType() == cmTarget::SHARED_LIBRARY
        || this->GetType() == cmTarget::STATIC_LIBRARY
        || this->GetType() == cmTarget::INTERFACE_LIBRARY)
      {
      // Shared libraries may have runtime implementation dependencies
      // on other shared libraries that are not in the interface.
      std::set<cmStdString> emitted;
      for(std::vector<std::string>::const_iterator
            li = iface.Libraries.begin(); li != iface.Libraries.end(); ++li)
        {
        emitted.insert(*li);
        }
      if (this->GetType() != cmTarget::INTERFACE_LIBRARY)
        {
        LinkImplementation const* impl = this->GetLinkImplementation(config,
                                                                  headTarget);
        for(std::vector<std::string>::const_iterator
              li = impl->Libraries.begin(); li != impl->Libraries.end(); ++li)
          {
          if(emitted.insert(*li).second)
            {
            if(cmTarget* tgt = this->Makefile->FindTargetToUse(li->c_str()))
              {
              // This is a runtime dependency on another shared library.
              if(tgt->GetType() == cmTarget::SHARED_LIBRARY)
                {
                iface.SharedDeps.push_back(*li);
                }
              }
            else
              {
              // TODO: Recognize shared library file names.  Perhaps this
              // should be moved to cmComputeLinkInformation, but that creates
              // a chicken-and-egg problem since this list is needed for its
              // construction.
              }
            }
          }
        if(this->LinkLanguagePropagatesToDependents())
          {
          // Targets using this archive need its language runtime libraries.
          iface.Languages = impl->Languages;
          }
        }
      }
    }
  else if (this->PolicyStatusCMP0022 == cmPolicies::WARN
        || this->PolicyStatusCMP0022 == cmPolicies::OLD)
    // If CMP0022 is NEW then the plain tll signature sets the
    // INTERFACE_LINK_LIBRARIES, so if we get here then the project
    // cleared the property explicitly and we should not fall back
    // to the link implementation.
    {
    // The link implementation is the default link interface.
    LinkImplementation const* impl = this->GetLinkImplementation(config,
                                                              headTarget);
    iface.ImplementationIsInterface = true;
    iface.Libraries = impl->Libraries;
    iface.WrongConfigLibraries = impl->WrongConfigLibraries;
    if(this->LinkLanguagePropagatesToDependents())
      {
      // Targets using this archive need its language runtime libraries.
      iface.Languages = impl->Languages;
      }

    if(this->PolicyStatusCMP0022 == cmPolicies::WARN &&
       !this->Internal->PolicyWarnedCMP0022)
      {
      // Compare the link implementation fallback link interface to the
      // preferred new link interface property and warn if different.
      cmListFileBacktrace lfbt;
      cmGeneratorExpression ge(lfbt);
      cmGeneratorExpressionDAGChecker dagChecker(lfbt, this->GetName(),
                                      "INTERFACE_LINK_LIBRARIES", 0, 0);
      std::vector<std::string> ifaceLibs;
      const char* newExplicitLibraries =
        this->GetProperty("INTERFACE_LINK_LIBRARIES");
      cmSystemTools::ExpandListArgument(
        ge.Parse(newExplicitLibraries)->Evaluate(this->Makefile,
                                                 config,
                                                 false,
                                                 headTarget,
                                                 this, &dagChecker),
        ifaceLibs);
      if (ifaceLibs != impl->Libraries)
        {
        std::string oldLibraries;
        std::string newLibraries;
        const char *sep = "";
        for(std::vector<std::string>::const_iterator it
              = impl->Libraries.begin(); it != impl->Libraries.end(); ++it)
          {
          oldLibraries += sep;
          oldLibraries += *it;
          sep = ";";
          }
        sep = "";
        for(std::vector<std::string>::const_iterator it
              = ifaceLibs.begin(); it != ifaceLibs.end(); ++it)
          {
          newLibraries += sep;
          newLibraries += *it;
          sep = ";";
          }
        if(oldLibraries.empty())
          { oldLibraries = "(empty)"; }
        if(newLibraries.empty())
          { newLibraries = "(empty)"; }

        cmOStringStream w;
        w <<
          (this->Makefile->GetPolicies()
           ->GetPolicyWarning(cmPolicies::CMP0022)) << "\n"
          "Target \"" << this->GetName() << "\" has an "
          "INTERFACE_LINK_LIBRARIES property.  "
          "This should be preferred as the source of the link interface "
          "for this library but because CMP0022 is not set CMake is "
          "ignoring the property and using the link implementation "
          "as the link interface instead."
          "\n"
          "INTERFACE_LINK_LIBRARIES:\n"
          "  " << newLibraries << "\n"
          "Link implementation:\n"
          "  " << oldLibraries << "\n";
        this->Makefile->IssueMessage(cmake::AUTHOR_WARNING, w.str());
        this->Internal->PolicyWarnedCMP0022 = true;
        }
      }
    }

  if(this->GetType() == cmTarget::STATIC_LIBRARY)
    {
    // How many repetitions are needed if this library has cyclic
    // dependencies?
    std::string propName = "LINK_INTERFACE_MULTIPLICITY";
    propName += suffix;
    if(const char* config_reps = this->GetProperty(propName.c_str()))
      {
      sscanf(config_reps, "%u", &iface.Multiplicity);
      }
    else if(const char* reps =
            this->GetProperty("LINK_INTERFACE_MULTIPLICITY"))
      {
      sscanf(reps, "%u", &iface.Multiplicity);
      }
    }

  return true;
}

//----------------------------------------------------------------------------
cmTarget::LinkImplementation const*
cmTarget::GetLinkImplementation(const char* config, cmTarget const* head) const
{
  // There is no link implementation for imported targets.
  if(this->IsImported())
    {
    return 0;
    }

  // Lookup any existing link implementation for this configuration.
  TargetConfigPair key(head, cmSystemTools::UpperCase(config? config : ""));

  cmTargetInternals::LinkImplMapType::iterator
    i = this->Internal->LinkImplMap.find(key);
  if(i == this->Internal->LinkImplMap.end())
    {
    // Compute the link implementation for this configuration.
    LinkImplementation impl;
    this->ComputeLinkImplementation(config, impl, head);

    // Store the information for this configuration.
    cmTargetInternals::LinkImplMapType::value_type entry(key, impl);
    i = this->Internal->LinkImplMap.insert(entry).first;
    }

  return &i->second;
}

//----------------------------------------------------------------------------
void cmTarget::ComputeLinkImplementation(const char* config,
                                         LinkImplementation& impl,
                                         cmTarget const* head) const
{
  // Collect libraries directly linked in this configuration.
  std::vector<std::string> llibs;
  this->GetDirectLinkLibraries(config, llibs, head);
  for(std::vector<std::string>::const_iterator li = llibs.begin();
      li != llibs.end(); ++li)
    {
    // Skip entries that resolve to the target itself or are empty.
    std::string item = this->CheckCMP0004(*li);
    if(item == this->GetName() || item.empty())
      {
      if(item == this->GetName())
        {
        bool noMessage = false;
        cmake::MessageType messageType = cmake::FATAL_ERROR;
        cmOStringStream e;
        switch(this->Makefile->GetPolicyStatus(cmPolicies::CMP0038))
          {
          case cmPolicies::WARN:
            {
            e << (this->Makefile->GetPolicies()
                  ->GetPolicyWarning(cmPolicies::CMP0038)) << "\n";
            messageType = cmake::AUTHOR_WARNING;
            }
            break;
          case cmPolicies::OLD:
            noMessage = true;
          case cmPolicies::REQUIRED_IF_USED:
          case cmPolicies::REQUIRED_ALWAYS:
          case cmPolicies::NEW:
            // Issue the fatal message.
            break;
          }

        if(!noMessage)
          {
          e << "Target \"" << this->GetName() << "\" links to itself.";
          this->Makefile->GetCMakeInstance()->IssueMessage(messageType,
                                                        e.str(),
                                                        this->GetBacktrace());
          if (messageType == cmake::FATAL_ERROR)
            {
            return;
            }
          }
        }
      continue;
      }
    cmTarget *tgt = this->Makefile->FindTargetToUse(li->c_str());

    if(!tgt && std::string(item).find("::") != std::string::npos)
      {
      bool noMessage = false;
      cmake::MessageType messageType = cmake::FATAL_ERROR;
      cmOStringStream e;
      switch(this->Makefile->GetPolicyStatus(cmPolicies::CMP0028))
        {
        case cmPolicies::WARN:
          {
          e << (this->Makefile->GetPolicies()
                ->GetPolicyWarning(cmPolicies::CMP0028)) << "\n";
          messageType = cmake::AUTHOR_WARNING;
          }
          break;
        case cmPolicies::OLD:
          noMessage = true;
        case cmPolicies::REQUIRED_IF_USED:
        case cmPolicies::REQUIRED_ALWAYS:
        case cmPolicies::NEW:
          // Issue the fatal message.
          break;
        }

      if(!noMessage)
        {
        e << "Target \"" << this->GetName() << "\" links to target \"" << item
          << "\" but the target was not found.  Perhaps a find_package() "
          "call is missing for an IMPORTED target, or an ALIAS target is "
          "missing?";
        this->Makefile->GetCMakeInstance()->IssueMessage(messageType,
                                                      e.str(),
                                                      this->GetBacktrace());
        if (messageType == cmake::FATAL_ERROR)
          {
          return;
          }
        }
      }

    // The entry is meant for this configuration.
    impl.Libraries.push_back(item);
    }

  cmTarget::LinkLibraryType linkType = this->ComputeLinkType(config);
  LinkLibraryVectorType const& oldllibs = this->GetOriginalLinkLibraries();
  for(cmTarget::LinkLibraryVectorType::const_iterator li = oldllibs.begin();
      li != oldllibs.end(); ++li)
    {
    if(li->second != cmTarget::GENERAL && li->second != linkType)
      {
      std::string item = this->CheckCMP0004(li->first);
      if(item == this->GetName() || item.empty())
        {
        continue;
        }
      // Support OLD behavior for CMP0003.
      impl.WrongConfigLibraries.push_back(item);
      }
    }

  // This target needs runtime libraries for its source languages.
  std::set<cmStdString> languages;
  // Get languages used in our source files.
  this->GetLanguages(languages);
  // Get languages used in object library sources.
  for(std::vector<std::string>::const_iterator
      i = this->ObjectLibraries.begin();
      i != this->ObjectLibraries.end(); ++i)
    {
    if(cmTarget* objLib = this->Makefile->FindTargetToUse(i->c_str()))
      {
      if(objLib->GetType() == cmTarget::OBJECT_LIBRARY)
        {
        objLib->GetLanguages(languages);
        }
      }
    }
  // Copy the set of langauges to the link implementation.
  for(std::set<cmStdString>::iterator li = languages.begin();
      li != languages.end(); ++li)
    {
    impl.Languages.push_back(*li);
    }
}

//----------------------------------------------------------------------------
std::string cmTarget::CheckCMP0004(std::string const& item) const
{
  // Strip whitespace off the library names because we used to do this
  // in case variables were expanded at generate time.  We no longer
  // do the expansion but users link to libraries like " ${VAR} ".
  std::string lib = item;
  std::string::size_type pos = lib.find_first_not_of(" \t\r\n");
  if(pos != lib.npos)
    {
    lib = lib.substr(pos, lib.npos);
    }
  pos = lib.find_last_not_of(" \t\r\n");
  if(pos != lib.npos)
    {
    lib = lib.substr(0, pos+1);
    }
  if(lib != item)
    {
    cmake* cm = this->Makefile->GetCMakeInstance();
    switch(this->PolicyStatusCMP0004)
      {
      case cmPolicies::WARN:
        {
        cmOStringStream w;
        w << (this->Makefile->GetPolicies()
              ->GetPolicyWarning(cmPolicies::CMP0004)) << "\n"
          << "Target \"" << this->GetName() << "\" links to item \""
          << item << "\" which has leading or trailing whitespace.";
        cm->IssueMessage(cmake::AUTHOR_WARNING, w.str(),
                         this->GetBacktrace());
        }
      case cmPolicies::OLD:
        break;
      case cmPolicies::NEW:
        {
        cmOStringStream e;
        e << "Target \"" << this->GetName() << "\" links to item \""
          << item << "\" which has leading or trailing whitespace.  "
          << "This is now an error according to policy CMP0004.";
        cm->IssueMessage(cmake::FATAL_ERROR, e.str(), this->GetBacktrace());
        }
        break;
      case cmPolicies::REQUIRED_IF_USED:
      case cmPolicies::REQUIRED_ALWAYS:
        {
        cmOStringStream e;
        e << (this->Makefile->GetPolicies()
              ->GetRequiredPolicyError(cmPolicies::CMP0004)) << "\n"
          << "Target \"" << this->GetName() << "\" links to item \""
          << item << "\" which has leading or trailing whitespace.";
        cm->IssueMessage(cmake::FATAL_ERROR, e.str(), this->GetBacktrace());
        }
        break;
      }
    }
  return lib;
}

//----------------------------------------------------------------------------
cmTargetInternalPointer::cmTargetInternalPointer()
{
  this->Pointer = new cmTargetInternals;
}

//----------------------------------------------------------------------------
cmTargetInternalPointer
::cmTargetInternalPointer(cmTargetInternalPointer const& r)
{
  // Ideally cmTarget instances should never be copied.  However until
  // we can make a sweep to remove that, this copy constructor avoids
  // allowing the resources (Internals) to be copied.
  this->Pointer = new cmTargetInternals(*r.Pointer);
}

//----------------------------------------------------------------------------
cmTargetInternalPointer::~cmTargetInternalPointer()
{
  deleteAndClear(this->Pointer->IncludeDirectoriesEntries);
  deleteAndClear(this->Pointer->CompileOptionsEntries);
  deleteAndClear(this->Pointer->CompileDefinitionsEntries);
  delete this->Pointer;
}

//----------------------------------------------------------------------------
cmTargetInternalPointer&
cmTargetInternalPointer::operator=(cmTargetInternalPointer const& r)
{
  if(this == &r) { return *this; } // avoid warning on HP about self check
  // Ideally cmTarget instances should never be copied.  However until
  // we can make a sweep to remove that, this copy constructor avoids
  // allowing the resources (Internals) to be copied.
  cmTargetInternals* oldPointer = this->Pointer;
  this->Pointer = new cmTargetInternals(*r.Pointer);
  delete oldPointer;
  return *this;
}
