/*============================================================================
  CMake - Cross Platform Makefile Generator
  Copyright 2000-2009 Kitware, Inc., Insight Software Consortium

  Distributed under the OSI-approved BSD License (the "License");
  see accompanying file Copyright.txt for details.

  This software is distributed WITHOUT ANY WARRANTY; without even the
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the License for more information.
============================================================================*/
#ifndef cmVisualStudioTargetGenerator_h
#define cmVisualStudioTargetGenerator_h
#include "cmStandardIncludes.h"

class cmTarget;
class cmMakefile;
class cmGeneratorTarget;
class cmGeneratedFileStream;
class cmGlobalVisualStudio10Generator;
class cmSourceFile;
class cmCustomCommand;
class cmLocalVisualStudio7Generator;
class cmComputeLinkInformation;
class cmVisualStudioGeneratorOptions;
struct cmIDEFlagTable;
#include "cmSourceGroup.h"

class cmVisualStudio10TargetGenerator
{
public:
  cmVisualStudio10TargetGenerator(cmTarget* target,
                                  cmGlobalVisualStudio10Generator* gg);
  ~cmVisualStudio10TargetGenerator();
  void Generate();
  // used by cmVisualStudioGeneratorOptions
  void WritePlatformConfigTag(
    const char* tag,
    const std::string& config,
    int indentLevel,
    const char* attribute = 0,
    const char* end = 0,
    std::ostream* strm = 0
    );

private:
  struct ToolSource
  {
    cmSourceFile const* SourceFile;
    bool RelativePath;
  };
  struct ToolSources: public std::vector<ToolSource> {};

  std::string ConvertPath(std::string const& path, bool forceRelative);
  void ConvertToWindowsSlash(std::string& s);
  void WriteString(const char* line, int indentLevel);
  void WriteProjectConfigurations();
  void WriteProjectConfigurationValues();
  void WriteMSToolConfigurationValues(std::string const& config);
  void WriteSource(const char* tool, cmSourceFile const* sf,
                   const char* end = 0);
  void WriteSources(const char* tool,
                    std::vector<cmSourceFile const*> const&);
  void WriteAllSources();
  void WriteDotNetReferences();
  void WriteEmbeddedResourceGroup();
  void WriteWinRTReferences();
  void WritePathAndIncrementalLinkOptions();
  void WriteItemDefinitionGroups();

  bool ComputeClOptions();
  bool ComputeClOptions(std::string const& configName);
  void WriteClOptions(std::string const& config,
                      std::vector<std::string> const & includes);
  bool ComputeRcOptions();
  bool ComputeRcOptions(std::string const& config);
  void WriteRCOptions(std::string const& config,
                      std::vector<std::string> const & includes);
  bool ComputeLinkOptions();
  bool ComputeLinkOptions(std::string const& config);
  void WriteLinkOptions(std::string const& config);
  void WriteMidlOptions(std::string const& config,
                        std::vector<std::string> const & includes);
  void OutputIncludes(std::vector<std::string> const & includes);
  void OutputLinkIncremental(std::string const& configName);
  void WriteCustomRule(cmSourceFile const* source,
                       cmCustomCommand const & command);
  void WriteCustomCommands();
  void WriteCustomCommand(cmSourceFile const* sf);
  void WriteGroups();
  void WriteProjectReferences();
  bool OutputSourceSpecificFlags(cmSourceFile const* source);
  void AddLibraries(cmComputeLinkInformation& cli, std::string& libstring);
  void WriteLibOptions(std::string const& config);
  void WriteEvents(std::string const& configName);
  void WriteEvent(const char* name,
                  std::vector<cmCustomCommand> const& commands,
                  std::string const& configName);
  void WriteGroupSources(const char* name, ToolSources const& sources,
                         std::vector<cmSourceGroup>& );
  void AddMissingSourceGroups(std::set<cmSourceGroup*>& groupsUsed,
                              const std::vector<cmSourceGroup>& allGroups);
  bool IsResxHeader(const std::string& headerFile);

  cmIDEFlagTable const* GetClFlagTable() const;
  cmIDEFlagTable const* GetRcFlagTable() const;
  cmIDEFlagTable const* GetLibFlagTable() const;
  cmIDEFlagTable const* GetLinkFlagTable() const;

private:
  typedef cmVisualStudioGeneratorOptions Options;
  typedef std::map<std::string, Options*> OptionsMap;
  OptionsMap ClOptions;
  OptionsMap RcOptions;
  OptionsMap LinkOptions;
  std::string PathToVcxproj;
  cmTarget* Target;
  cmGeneratorTarget* GeneratorTarget;
  cmMakefile* Makefile;
  std::string Platform;
  std::string GUID;
  std::string Name;
  cmGlobalVisualStudio10Generator* GlobalGenerator;
  cmGeneratedFileStream* BuildFileStream;
  cmLocalVisualStudio7Generator* LocalGenerator;
  std::set<cmSourceFile const*> SourcesVisited;

  typedef std::map<std::string, ToolSources> ToolSourceMap;
  ToolSourceMap Tools;
};

#endif
