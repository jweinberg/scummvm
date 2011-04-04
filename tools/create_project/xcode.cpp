/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * $URL$
 * $Id$
 *
 */

#include "xcode.h"

#include <fstream>
#include <algorithm>

#if defined(_WIN32) || defined(WIN32)
#include <windows.h>
#else
#include <sys/param.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#endif

namespace CreateProjectTool {

#define DEBUG_XCODE_HASH 0

#define ADD_DEFINE(defines, name) \
	defines.push_back(name);

#define ADD_SETTING(config, key, value) \
	config.settings[key] = Setting(value, "", SettingsNoQuote);

#define ADD_SETTING_ORDER(config, key, value, order) \
	config.settings[key] = Setting(value, "", SettingsNoQuote, 0, order);

#define ADD_SETTING_ORDER_NOVALUE(config, key, comment, order) \
	config.settings[key] = Setting("", comment, SettingsNoValue, 0, order);

#define ADD_SETTING_QUOTE(config, key, value) \
	config.settings[key] = Setting(value);

#define ADD_SETTING_QUOTE_VAR(config, key, value) \
	config.settings[key] = Setting(value, "", SettingsQuoteVariable);

#define ADD_SETTING_LIST(config, key, values, flags, indent) \
	config.settings[key] = Setting(values, flags, indent);

#define REMOVE_SETTING(config, key) \
	config.settings.erase(key);

#define ADD_BUILD_FILE(id, name, comment) { \
	Object *buildFile = new Object(this, id, name, "PBXBuildFile", "PBXBuildFile", comment); \
	buildFile->addProperty("fileRef", getHash(name), name, SettingsNoValue); \
	_buildFile.add(buildFile); \
	_buildFile.flags = SettingsSingleItem; \
}

#define ADD_FILE_REFERENCE(name, properties) { \
	Object *fileRef = new Object(this, name, name, "PBXFileReference", "PBXFileReference", name); \
	if (!properties.fileEncoding.empty()) fileRef->addProperty("fileEncoding", properties.fileEncoding, "", SettingsNoValue); \
	if (!properties.lastKnownFileType.empty()) fileRef->addProperty("lastKnownFileType", properties.lastKnownFileType, "", SettingsNoValue); \
	if (!properties.fileName.empty()) fileRef->addProperty("name", properties.fileName, "", SettingsNoValue); \
	if (!properties.filePath.empty()) fileRef->addProperty("path", properties.filePath, "", SettingsNoValue); \
	if (!properties.sourceTree.empty()) fileRef->addProperty("sourceTree", properties.sourceTree, "", SettingsNoValue); \
	_fileReference.add(fileRef); \
	_fileReference.flags = SettingsSingleItem; \
}

XCodeProvider::XCodeProvider(StringList &global_warnings, std::map<std::string, StringList> &project_warnings, const int version)
	: ProjectProvider(global_warnings, project_warnings, version) {
}

void XCodeProvider::createWorkspace(const BuildSetup &setup) {
	// Create project folder
	std::string workspace = setup.outputDir + '/' + "scummvm.xcodeproj";

#if defined(_WIN32) || defined(WIN32)
	if (!CreateDirectory(workspace.c_str(), NULL))
		if (GetLastError() != ERROR_ALREADY_EXISTS)
			error("Could not create folder \"" + setup.outputDir + '/' + "scummvm.xcodeproj\"");
#else
	if (mkdir(workspace.c_str(), 0777) == -1) {
		if (errno == EEXIST) {
			// Try to open as a folder (might be a file / symbolic link)
			DIR *dirp = opendir(workspace.c_str());
			if (dirp == NULL) {
				error("Could not create folder \"" + setup.outputDir + '/' + "scummvm.xcodeproj\"");
			} else {
				// The folder exists, just close the stream and return
				closedir(dirp);
			}
		} else {
			error("Could not create folder \"" + setup.outputDir + '/' + "scummvm.xcodeproj\"");
		}
	}
#endif

	// Setup global objects
	setupDefines(setup);
	SET_VALUES(_targets, "ScummVM-iPhone", "ScummVM-OS X", "ScummVM-Simulator");

	setupCopyFilesBuildPhase();
	setupFrameworksBuildPhase();
	setupNativeTarget();
	setupProject();
	setupResourcesBuildPhase();
	setupBuildConfiguration();

	// Init remaining lists
	_buildFile.comment = "PBXBuildFile";
	_fileReference.comment = "PBXFileReference";
	_groups.comment = "PBXGroup";
}

// We are done with constructing all the object graph and we got through every project, output the main project file
// (this is kind of a hack since other providers use separate project files)
void XCodeProvider::createOtherBuildFiles(const BuildSetup &setup) {
	// This needs to be done at the end when all build files have been accounted for
	setupSourcesBuildPhase();

	ouputMainProjectFile(setup);
}

// Store information about a project here, for use at the end
void XCodeProvider::createProjectFile(const std::string &, const std::string &, const BuildSetup &setup, const std::string &moduleDir,
                                      const StringList &includeList, const StringList &excludeList) {
	std::string modulePath;
	if (!moduleDir.compare(0, setup.srcDir.size(), setup.srcDir)) {
		modulePath = moduleDir.substr(setup.srcDir.size());
		if (!modulePath.empty() && modulePath.at(0) == '/')
			modulePath.erase(0, 1);
	}

	std::ofstream project;
	if (modulePath.size())
		addFilesToProject(moduleDir, project, includeList, excludeList, setup.filePrefix + '/' + modulePath);
	else
		addFilesToProject(moduleDir, project, includeList, excludeList, setup.filePrefix);
}

void XCodeProvider::writeFileListToProject(const FileNode &dir, std::ofstream &projectFile, const int indentation,
                                           const StringList &duplicate, const std::string &objPrefix, const std::string &filePrefix) {

	for (FileNode::NodeList::const_iterator i = dir.children.begin(); i != dir.children.end(); ++i) {
		const FileNode *node = *i;

		if (!node->children.empty()) {
			writeFileListToProject(*node, projectFile, indentation + 1, duplicate, objPrefix + node->name + '_', filePrefix + node->name + '/');
		} else {
			std::string name, ext;
			splitFilename(node->name, name, ext);

			FileProperty properties("", "", node->name, "\"<group>\"");
			properties.fileEncoding = "4";

			// Store as build file if source file
			if (ext == "cpp") {
				for (unsigned int i = 0; i < _targets.size(); i++) {
					ADD_BUILD_FILE("PBXBuildFile_" + node->name + _targets[i], node->name, node->name + " in Sources");
				}

				properties.lastKnownFileType = "sourcecode.cpp.cpp";

				ADD_FILE_REFERENCE(node->name, properties);
			} else if (ext == "h") {
				properties.lastKnownFileType = "sourcecode.c.h";

				ADD_FILE_REFERENCE(node->name, properties);
			}

			// TODO: Store group info here
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// Main Project file
//////////////////////////////////////////////////////////////////////////
void XCodeProvider::ouputMainProjectFile(const BuildSetup &setup) {
	std::ofstream project((setup.outputDir + '/' + "scummvm.xcodeproj" + '/' + "project.pbxproj").c_str());
	if (!project)
		error("Could not open \"" + setup.outputDir + '/' + "scummvm.xcodeproj" + '/' + "project.pbxproj\" for writing");

	//////////////////////////////////////////////////////////////////////////
	// Header
	project << "// !$*UTF8*$!\n"
	           "{\n"
	           "\t" << writeSetting("archiveVersion", "1", "", SettingsNoQuote) << ";\n"
	           "\tclasses = {\n"
	           "\t};\n"
	           "\t" << writeSetting("objectVersion", "45", "", SettingsNoQuote) << ";\n"
	           "\tobjects = {\n";

	//////////////////////////////////////////////////////////////////////////
	// List of objects
	project << _buildFile.toString();
	project << _copyFilesBuildPhase.toString();
	project << _fileReference.toString();
	project << _frameworksBuildPhase.toString();
	project << _groups.toString();
	project << _nativeTarget.toString();
	project << _project.toString();
	project << _resourcesBuildPhase.toString();
	project << _sourcesBuildPhase.toString();
	project << _buildConfiguration.toString();
	project << _configurationList.toString();

	//////////////////////////////////////////////////////////////////////////
	// Footer
	project << "\t};\n"
	           "\t" << writeSetting("rootObject", getHash("PBXProject"), "Project object", SettingsNoQuote) << ";\n"
	           "}";

}

//////////////////////////////////////////////////////////////////////////
// Setup functions
//////////////////////////////////////////////////////////////////////////
void XCodeProvider::setupCopyFilesBuildPhase() {
	_copyFilesBuildPhase.comment = "PBXCopyFilesBuildPhase";

	// Same as for containers: a rule for each native target
	for (unsigned int i = 0; i < _targets.size(); i++) {
		Object *container = new Object(this, "PBXCopyFilesBuildPhase_" + _targets[i], "PBXCopyFilesBuildPhase", "PBXCopyFilesBuildPhase", "", "CopyFiles");

		container->addProperty("buildActionMask", "2147483647", "", SettingsNoValue);
		container->addProperty("dstPath", "", "", SettingsNoValue|SettingsQuoteVariable);
		container->addProperty("dstSubfolderSpec", "7", "", SettingsNoValue);
		container->addProperty("files", "", "", SettingsNoValue|SettingsAsList);
		container->addProperty("runOnlyForDeploymentPostprocessing", "0", "", SettingsNoValue);

		_copyFilesBuildPhase.add(container);
	}
}

/**
 * Sets up the frameworks build phase.
 *
 * (each native target has different build rules)
 */
void XCodeProvider::setupFrameworksBuildPhase() {
	_frameworksBuildPhase.comment = "PBXFrameworksBuildPhase";

	// Setup framework file properties
	std::map<std::string, FileProperty> properties;

	// Frameworks
	properties["ApplicationServices.framework"] = FileProperty("wrapper.framework", "ApplicationServices.framework", "System/Library/Frameworks/ApplicationServices.framework", "SDKROOT");
	properties["AudioToolbox.framework"]        = FileProperty("wrapper.framework", "AudioToolbox.framework", "System/Library/Frameworks/AudioToolbox.framework", "SDKROOT");
	properties["AudioUnit.framework"]           = FileProperty("wrapper.framework", "AudioUnit.framework", "System/Library/Frameworks/AudioUnit.framework", "SDKROOT");
	properties["Carbon.framework"]              = FileProperty("wrapper.framework", "Carbon.framework", "System/Library/Frameworks/Carbon.framework", "SDKROOT");
	properties["Cocoa.framework"]               = FileProperty("wrapper.framework", "Cocoa.framework", "System/Library/Frameworks/Cocoa.framework", "SDKROOT");
	properties["CoreAudio.framework"]           = FileProperty("wrapper.framework", "CoreAudio.framework", "System/Library/Frameworks/CoreAudio.framework", "SDKROOT");
	properties["CoreFoundation.framework"]      = FileProperty("wrapper.framework", "CoreFoundation.framework", "System/Library/Frameworks/CoreFoundation.framework", "SDKROOT");
	properties["CoreMIDI.framework"]            = FileProperty("wrapper.framework", "CoreMIDI.framework", "System/Library/Frameworks/CoreMIDI.framework", "SDKROOT");
	properties["Foundation.framework"]          = FileProperty("wrapper.framework", "Foundation.framework", "System/Library/Frameworks/Foundation.framework", "SDKROOT");
	properties["IOKit.framework"]               = FileProperty("wrapper.framework", "IOKit.framework", "System/Library/Frameworks/IOKit.framework", "SDKROOT");
	properties["OpenGLES.framework"]            = FileProperty("wrapper.framework", "OpenGLES.framework", "System/Library/Frameworks/OpenGLES.framework", "SDKROOT");
	properties["QuartzCore.framework"]          = FileProperty("wrapper.framework", "QuartzCore.framework", "System/Library/Frameworks/QuartzCore.framework", "SDKROOT");
	properties["QuickTime.framework"]           = FileProperty("wrapper.framework", "QuickTime.framework", "System/Library/Frameworks/QuickTime.framework", "SDKROOT");
	properties["UIKit.framework"]               = FileProperty("wrapper.framework", "UIKit.framework", "System/Library/Frameworks/UIKit.framework", "SDKROOT");

	// Local libraries
	properties["libFLAC.a"]                     = FileProperty("archive.ar", "libFLAC.a", "lib/libFLAC.a", "\"<group>\"");
	properties["libmad.a"]                      = FileProperty("archive.ar", "libmad.a", "lib/libmad.a", "\"<group>\"");
	properties["libmpeg2.a"]                    = FileProperty("archive.ar", "libmpeg2.a", "lib/libmpeg2.a", "\"<group>\"");
	properties["libvorbisidec.a"]               = FileProperty("archive.ar", "libvorbisidec.a", "lib/libvorbisidec.a", "\"<group>\"");

	//////////////////////////////////////////////////////////////////////////
	// iPhone
	Object *framework_iPhone = new Object(this, "PBXFrameworksBuildPhase_" + _targets[0], "PBXFrameworksBuildPhase", "PBXFrameworksBuildPhase", "", "Frameworks");

	framework_iPhone->addProperty("buildActionMask", "2147483647", "", SettingsNoValue);
	framework_iPhone->addProperty("runOnlyForDeploymentPostprocessing", "0", "", SettingsNoValue);

	// List of frameworks
	Property iPhone_files;
	iPhone_files.hasOrder = true;
	iPhone_files.flags = SettingsAsList;

	ValueList frameworks_iPhone;
	SET_VALUES(frameworks_iPhone, "CoreAudio.framework", "CoreFoundation.framework", "Foundation.framework", "UIKit.framework",
	                              "AudioToolbox.framework", "QuartzCore.framework", "libmad.a", "libFLAC.a", "libvorbisidec.a",
	                              "libmpeg2.a", "OpenGLES.framework");

	int order = 0;
	for (ValueList::iterator framework = frameworks_iPhone.begin(); framework != frameworks_iPhone.end(); framework++) {
		std::string id = "Frameworks_" + *framework + "_iphone";
		std::string comment = *framework + " in Frameworks";

		ADD_SETTING_ORDER_NOVALUE(iPhone_files, getHash(id), comment, order++);
		ADD_BUILD_FILE(id, *framework, comment);
		ADD_FILE_REFERENCE(*framework, properties[*framework]);
	}

	framework_iPhone->properties["files"] = iPhone_files;

	_frameworksBuildPhase.add(framework_iPhone);

	//////////////////////////////////////////////////////////////////////////
	// ScummVM-OS X
	Object *framework_OSX = new Object(this, "PBXFrameworksBuildPhase_" + _targets[1], "PBXFrameworksBuildPhase", "PBXFrameworksBuildPhase", "", "Frameworks");

	framework_OSX->addProperty("buildActionMask", "2147483647", "", SettingsNoValue);
	framework_OSX->addProperty("runOnlyForDeploymentPostprocessing", "0", "", SettingsNoValue);

	// List of frameworks
	Property osx_files;
	osx_files.hasOrder = true;
	osx_files.flags = SettingsAsList;

	ValueList frameworks_osx;
	SET_VALUES(frameworks_osx, "CoreFoundation.framework", "Foundation.framework", "AudioToolbox.framework", "QuickTime.framework",
	                           "CoreMIDI.framework", "CoreAudio.framework", "QuartzCore.framework", "Carbon.framework",
	                           "ApplicationServices.framework", "IOKit.framework", "Cocoa.framework", "AudioUnit.framework");

	order = 0;
	for (ValueList::iterator framework = frameworks_osx.begin(); framework != frameworks_osx.end(); framework++) {
		std::string id = "Frameworks_" + *framework + "_osx";
		std::string comment = *framework + " in Frameworks";

		ADD_SETTING_ORDER_NOVALUE(osx_files, getHash(id), comment, order++);
		ADD_BUILD_FILE(id, *framework, comment);
		ADD_FILE_REFERENCE(*framework, properties[*framework]);
	}

	framework_OSX->properties["files"] = osx_files;

	_frameworksBuildPhase.add(framework_OSX);

	//////////////////////////////////////////////////////////////////////////
	// Simulator
	Object *framework_simulator = new Object(this, "PBXFrameworksBuildPhase_" + _targets[2], "PBXFrameworksBuildPhase", "PBXFrameworksBuildPhase", "", "Frameworks");

	framework_simulator->addProperty("buildActionMask", "2147483647", "", SettingsNoValue);
	framework_simulator->addProperty("runOnlyForDeploymentPostprocessing", "0", "", SettingsNoValue);

	// List of frameworks
	Property simulator_files;
	simulator_files.hasOrder = true;
	simulator_files.flags = SettingsAsList;

	ValueList frameworks_simulator;
	SET_VALUES(frameworks_simulator, "CoreAudio.framework", "CoreFoundation.framework", "Foundation.framework", "UIKit.framework",
	                                 "AudioToolbox.framework", "QuartzCore.framework", "OpenGLES.framework");

	order = 0;
	for (ValueList::iterator framework = frameworks_simulator.begin(); framework != frameworks_simulator.end(); framework++) {
		std::string id = "Frameworks_" + *framework + "_simulator";
		std::string comment = *framework + " in Frameworks";

		ADD_SETTING_ORDER_NOVALUE(simulator_files, getHash(id), comment, order++);
		ADD_BUILD_FILE(id, *framework, comment);
		ADD_FILE_REFERENCE(*framework, properties[*framework]);
	}

	framework_simulator->properties["files"] = simulator_files;

	_frameworksBuildPhase.add(framework_simulator);
}

void XCodeProvider::setupNativeTarget() {
	_nativeTarget.comment = "PBXNativeTarget";

	// Output native target section
	for (unsigned int i = 0; i < _targets.size(); i++) {
		Object *target = new Object(this, "PBXNativeTarget_" + _targets[i], "PBXNativeTarget", "PBXNativeTarget", "", _targets[i]);

		target->addProperty("buildConfigurationList", getHash("XCConfigurationList_" + _targets[i]), "Build configuration list for PBXNativeTarget \"" + _targets[i] + "\"", SettingsNoValue);

		Property buildPhases;
		buildPhases.hasOrder = true;
		buildPhases.flags = SettingsAsList;
		buildPhases.settings[getHash("PBXResourcesBuildPhase_" + _targets[i])] = Setting("", "Resources", SettingsNoValue, 0, 0);
		buildPhases.settings[getHash("PBXSourcesBuildPhase_" + _targets[i])] = Setting("", "Sources", SettingsNoValue, 0, 1);
		buildPhases.settings[getHash("PBXFrameworksBuildPhase_" + _targets[i])] = Setting("", "Frameworks", SettingsNoValue, 0, 2);
		buildPhases.settings[getHash("PBXCopyFilesBuildPhase_" + _targets[i])] = Setting("", "CopyFiles", SettingsNoValue, 0, 3);
		target->properties["buildPhases"] = buildPhases;

		target->addProperty("buildRules", "", "", SettingsNoValue|SettingsAsList);

		target->addProperty("dependencies", "", "", SettingsNoValue|SettingsAsList);

		target->addProperty("name", _targets[i], "", SettingsNoValue|SettingsQuoteVariable);
		target->addProperty("productName", "scummvm", "", SettingsNoValue);
		target->addProperty("productReference", getHash("PBXFileReference_ScummVM.app_" + _targets[i]), "ScummVM.app", SettingsNoValue);
		target->addProperty("productType", "com.apple.product-type.application", "", SettingsNoValue|SettingsQuoteVariable);

		_nativeTarget.add(target);
	}
}

void XCodeProvider::setupProject() {
	_project.comment = "PBXProject";

	Object *project = new Object(this, "PBXProject", "PBXProject", "PBXProject", "", "Project object");

	project->addProperty("buildConfigurationList", getHash("XCConfigurationList_scummvm"), "Build configuration list for PBXProject \"scummvm\"", SettingsNoValue);
	project->addProperty("compatibilityVersion", "Xcode 3.1", "", SettingsNoValue|SettingsQuoteVariable);
	project->addProperty("developmentRegion", "English", "", SettingsNoValue);
	project->addProperty("hasScannedForEncodings", "1", "", SettingsNoValue);

	// List of known regions
	Property regions;
	regions.flags = SettingsAsList;
	ADD_SETTING_ORDER_NOVALUE(regions, "English", "", 0);
	ADD_SETTING_ORDER_NOVALUE(regions, "Japanese", "", 1);
	ADD_SETTING_ORDER_NOVALUE(regions, "French", "", 2);
	ADD_SETTING_ORDER_NOVALUE(regions, "German", "", 3);
	project->properties["knownRegions"] = regions;

	project->addProperty("mainGroup", getHash("PBXGroup_CustomTemplate"), "CustomTemplate", SettingsNoValue);
	project->addProperty("projectDirPath", "", "", SettingsNoValue|SettingsQuoteVariable);
	project->addProperty("projectRoot", "", "", SettingsNoValue|SettingsQuoteVariable);

	// List of targets
	Property targets;
	targets.flags = SettingsAsList;
	targets.settings[getHash("PBXNativeTarget_" + _targets[0])] = Setting("", _targets[0], SettingsNoValue, 0, 0);
	targets.settings[getHash("PBXNativeTarget_" + _targets[1])] = Setting("", _targets[1], SettingsNoValue, 0, 1);
	targets.settings[getHash("PBXNativeTarget_" + _targets[2])] = Setting("", _targets[2], SettingsNoValue, 0, 2);
	project->properties["targets"] = targets;

	_project.add(project);
}

void XCodeProvider::setupResourcesBuildPhase() {
	_resourcesBuildPhase.comment = "PBXResourcesBuildPhase";

	// Setup resource file properties
	std::map<std::string, FileProperty> properties;
	properties["scummclassic.zip"] = FileProperty("archive.zip", "", "scummclassic.zip", "\"<group>\"");
	properties["scummmodern.zip"]  = FileProperty("archive.zip", "", "scummmodern.zip", "\"<group>\"");

	properties["kyra.dat"]         = FileProperty("file", "", "kyra.dat", "\"<group>\"");
	properties["lure.dat"]         = FileProperty("file", "", "lure.dat", "\"<group>\"");
	properties["queen.tbl"]        = FileProperty("file", "", "queen.tbl", "\"<group>\"");
	properties["sky.cpt"]          = FileProperty("file", "", "sky.cpt", "\"<group>\"");
	properties["drascula.dat"]     = FileProperty("file", "", "drascula.dat", "\"<group>\"");
	properties["hugo.dat"]         = FileProperty("file", "", "hugo.dat", "\"<group>\"");
	properties["m4.dat"]           = FileProperty("file", "", "m4.dat", "\"<group>\"");
	properties["teenagent.dat"]    = FileProperty("file", "", "teenagent.dat", "\"<group>\"");
	properties["toon.dat"]         = FileProperty("file", "", "toon.dat", "\"<group>\"");

	properties["Default.png"]      = FileProperty("image.png", "", "Default.png", "\"<group>\"");
	properties["icon.png"]         = FileProperty("image.png", "", "icon.png", "\"<group>\"");
	properties["icon-72.png"]      = FileProperty("image.png", "", "icon-72.png", "\"<group>\"");
	properties["icon4.png"]        = FileProperty("image.png", "", "icon4.png", "\"<group>\"");

	// Same as for containers: a rule for each native target
	for (unsigned int i = 0; i < _targets.size(); i++) {
		Object *resource = new Object(this, "PBXResourcesBuildPhase_" + _targets[i], "PBXResourcesBuildPhase", "PBXResourcesBuildPhase", "", "Resources");

		resource->addProperty("buildActionMask", "2147483647", "", SettingsNoValue);

		// Add default files
		Property files;
		files.hasOrder = true;
		files.flags = SettingsAsList;

		ValueList files_list;
		SET_VALUES(files_list, "scummclassic.zip", "scummmodern.zip", "kyra.dat", "lure.dat", "queen.tbl", "sky.cpt", "Default.png", "icon.png", "icon-72.png", "icon4.png", "drascula.dat", "hugo.dat", "m4.dat", "teenagent.dat", "toon.dat");

		int order = 0;
		for (ValueList::iterator file = files_list.begin(); file != files_list.end(); file++) {
			std::string id = "PBXResources_" + *file;
			std::string comment = *file + " in Resources";

			ADD_SETTING_ORDER_NOVALUE(files, getHash(id), comment, order++);
			ADD_BUILD_FILE(id, *file, comment);
			ADD_FILE_REFERENCE(*file, properties[*file]);
		}

		// Add custom files depending on the target
		if (_targets[i] == "ScummVM-OS X") {
			files.settings[getHash("PBXResources_scummvm.icns")] = Setting("", "scummvm.icns in Resources", SettingsNoValue, 0, 6);

			// Remove 2 iphone icon files
			files.settings.erase(getHash("PBXResources_Default.png"));
			files.settings.erase(getHash("PBXResources_icon.png"));
		}

		resource->properties["files"] = files;

		resource->addProperty("runOnlyForDeploymentPostprocessing", "0", "", SettingsNoValue);

		_resourcesBuildPhase.add(resource);
	}
}

void XCodeProvider::setupSourcesBuildPhase() {
	_sourcesBuildPhase.comment = "PBXSourcesBuildPhase";

	// Each target has a set of sources files
	for (unsigned int i = 0; i < _targets.size(); i++) {
		Object *sources = new Object(this, "PBXSourcesBuildPhase_" + _targets[i], "PBXSourcesBuildPhase", "PBXSourcesBuildPhase", "", "Sources");

		sources->addProperty("buildActionMask", "2147483647", "", SettingsNoValue);

		// Add the list of sources files (.cpp)
		Property files;
		files.flags = SettingsAsList;

		for (std::vector<Object *>::iterator file = _buildFile.objects.begin(); file != _buildFile.objects.end(); file++) {
			std::string name, ext;
			splitFilename((*file)->name, name, ext);

			if (ext == "cpp")
				files.settings[getHash((*file)->id)] = Setting("",  (*file)->name + " in Sources", SettingsNoQuote);
		}

		sources->properties["files"] = files;

		sources->addProperty("runOnlyForDeploymentPostprocessing", "0", "", SettingsNoValue);

		_sourcesBuildPhase.add(sources);
	}
}

// Setup all build configurations
void XCodeProvider::setupBuildConfiguration() {

	_buildConfiguration.comment = "XCBuildConfiguration";
	_buildConfiguration.flags = SettingsAsList;

	/****************************************
	 * iPhone
	 ****************************************/

	// Debug
	Object *iPhone_Debug_Object = new Object(this, "XCBuildConfiguration_ScummVM-iPhone_Debug", _targets[0] /* ScummVM-iPhone */, "XCBuildConfiguration", "PBXNativeTarget", "Debug");
	Property iPhone_Debug;
	ADD_SETTING_QUOTE(iPhone_Debug, "ARCHS", "$(ARCHS_UNIVERSAL_IPHONE_OS)");
	ADD_SETTING_QUOTE(iPhone_Debug, "CODE_SIGN_IDENTITY", "iPhone Developer");
	ADD_SETTING_QUOTE_VAR(iPhone_Debug, "CODE_SIGN_IDENTITY[sdk=iphoneos*]", "iPhone Developer");
	ADD_SETTING(iPhone_Debug, "COMPRESS_PNG_FILES", "NO");
	ADD_SETTING(iPhone_Debug, "COPY_PHASE_STRIP", "NO");
	ADD_SETTING_QUOTE(iPhone_Debug, "DEBUG_INFORMATION_FORMAT", "dwarf-with-dsym");
	ValueList iPhone_FrameworkSearchPaths;
	SET_VALUES(iPhone_FrameworkSearchPaths, "$(inherited)", "\"$(SDKROOT)$(SYSTEM_LIBRARY_DIR)/PrivateFrameworks\"");
	ADD_SETTING_LIST(iPhone_Debug, "FRAMEWORK_SEARCH_PATHS", iPhone_FrameworkSearchPaths, SettingsAsList, 5);
	ADD_SETTING(iPhone_Debug, "GCC_DYNAMIC_NO_PIC", "NO");
	ADD_SETTING(iPhone_Debug, "GCC_ENABLE_CPP_EXCEPTIONS", "NO");
	ADD_SETTING(iPhone_Debug, "GCC_ENABLE_FIX_AND_CONTINUE", "NO");
	ADD_SETTING(iPhone_Debug, "GCC_OPTIMIZATION_LEVEL", "0");
	ADD_SETTING(iPhone_Debug, "GCC_PRECOMPILE_PREFIX_HEADER", "NO");
	ADD_SETTING_QUOTE(iPhone_Debug, "GCC_PREFIX_HEADER", "");
	ADD_SETTING(iPhone_Debug, "GCC_THUMB_SUPPORT", "NO");
	ADD_SETTING(iPhone_Debug, "GCC_UNROLL_LOOPS", "YES");
	ValueList iPhone_HeaderSearchPaths;
	SET_VALUES(iPhone_HeaderSearchPaths, "../../engines/", "../../", "include/");
	ADD_SETTING_LIST(iPhone_Debug, "HEADER_SEARCH_PATHS", iPhone_HeaderSearchPaths, SettingsAsList|SettingsNoQuote, 5);
	ADD_SETTING(iPhone_Debug, "INFOPLIST_FILE", "Info.plist");
	ValueList iPhone_LibPaths;
	SET_VALUES(iPhone_LibPaths, "$(inherited)", "\"$(SRCROOT)/lib\"");
	ADD_SETTING_LIST(iPhone_Debug, "LIBRARY_SEARCH_PATHS", iPhone_LibPaths, SettingsAsList, 5);
	ADD_SETTING(iPhone_Debug, "ONLY_ACTIVE_ARCH", "YES");
	ADD_SETTING(iPhone_Debug, "PREBINDING", "NO");
	ADD_SETTING(iPhone_Debug, "PRODUCT_NAME", "ScummVM");
	ADD_SETTING_QUOTE(iPhone_Debug, "PROVISIONING_PROFILE", "EF590570-5FAC-4346-9071-D609DE2B28D8");
	ADD_SETTING_QUOTE_VAR(iPhone_Debug, "PROVISIONING_PROFILE[sdk=iphoneos*]", "");
	ADD_SETTING(iPhone_Debug, "SDKROOT", "iphoneos4.0");
	ADD_SETTING_QUOTE(iPhone_Debug, "TARGETED_DEVICE_FAMILY", "1,2");

	iPhone_Debug_Object->addProperty("name", "Debug", "", SettingsNoValue);
	iPhone_Debug_Object->properties["buildSettings"] = iPhone_Debug;

	// Release
	Object *iPhone_Release_Object = new Object(this, "XCBuildConfiguration_ScummVM-iPhone_Release", _targets[0] /* ScummVM-iPhone */, "XCBuildConfiguration", "PBXNativeTarget", "Release");
	Property iPhone_Release(iPhone_Debug);
	ADD_SETTING(iPhone_Release, "GCC_OPTIMIZATION_LEVEL", "3");
	ADD_SETTING(iPhone_Release, "COPY_PHASE_STRIP", "YES");
	REMOVE_SETTING(iPhone_Release, "GCC_DYNAMIC_NO_PIC");
	ADD_SETTING(iPhone_Release, "WRAPPER_EXTENSION", "app");

	iPhone_Release_Object->addProperty("name", "Release", "", SettingsNoValue);
	iPhone_Release_Object->properties["buildSettings"] = iPhone_Release;

	_buildConfiguration.add(iPhone_Debug_Object);
	_buildConfiguration.add(iPhone_Release_Object);

	/****************************************
	 * scummvm
	 ****************************************/

	// Debug
	Object *scummvm_Debug_Object = new Object(this, "XCBuildConfiguration_scummvm_Debug", "scummvm", "XCBuildConfiguration", "PBXProject", "Debug");
	Property scummvm_Debug;
	ADD_SETTING(scummvm_Debug, "ALWAYS_SEARCH_USER_PATHS", "NO");
	ADD_SETTING_QUOTE(scummvm_Debug, "ARCHS", "$(ARCHS_STANDARD_32_BIT)");
	ADD_SETTING_QUOTE(scummvm_Debug, "CODE_SIGN_IDENTITY", "Don't Code Sign");
	ADD_SETTING_QUOTE_VAR(scummvm_Debug, "CODE_SIGN_IDENTITY[sdk=iphoneos*]", "Don't Code Sign");
	ADD_SETTING_QUOTE(scummvm_Debug, "FRAMEWORK_SEARCH_PATHS", "");
	ADD_SETTING(scummvm_Debug, "GCC_C_LANGUAGE_STANDARD", "c99");
	ADD_SETTING(scummvm_Debug, "GCC_ENABLE_CPP_EXCEPTIONS", "NO");
	ADD_SETTING(scummvm_Debug, "GCC_ENABLE_CPP_RTTI", "NO");
	ADD_SETTING(scummvm_Debug, "GCC_INPUT_FILETYPE", "automatic");
	ADD_SETTING(scummvm_Debug, "GCC_OPTIMIZATION_LEVEL", "0");
	ValueList scummvm_defines(_defines);
	ADD_DEFINE(scummvm_defines, "IPHONE");
	ADD_DEFINE(scummvm_defines, "XCODE");
	ADD_DEFINE(scummvm_defines, "IPHONE_OFFICIAL");
	ADD_SETTING_LIST(scummvm_Debug, "GCC_PREPROCESSOR_DEFINITIONS", scummvm_defines, SettingsNoQuote|SettingsAsList, 5);
	ADD_SETTING(scummvm_Debug, "GCC_THUMB_SUPPORT", "NO");
	ADD_SETTING(scummvm_Debug, "GCC_USE_GCC3_PFE_SUPPORT", "NO");
	ADD_SETTING(scummvm_Debug, "GCC_WARN_ABOUT_RETURN_TYPE", "YES");
	ADD_SETTING(scummvm_Debug, "GCC_WARN_UNUSED_VARIABLE", "YES");
	ValueList scummvm_HeaderPaths;
	SET_VALUES(scummvm_HeaderPaths, "include/", "../../engines/", "../../");
	ADD_SETTING_LIST(scummvm_Debug, "HEADER_SEARCH_PATHS", scummvm_HeaderPaths, SettingsNoQuote|SettingsAsList, 5);
	ADD_SETTING_QUOTE(scummvm_Debug, "LIBRARY_SEARCH_PATHS", "");
	ADD_SETTING(scummvm_Debug, "ONLY_ACTIVE_ARCH", "YES");
	ADD_SETTING_QUOTE(scummvm_Debug, "OTHER_CFLAGS", "");
	ADD_SETTING_QUOTE(scummvm_Debug, "OTHER_LDFLAGS", "-lz");
	ADD_SETTING(scummvm_Debug, "PREBINDING", "NO");
	ADD_SETTING(scummvm_Debug, "SDKROOT", "macosx10.6");

	scummvm_Debug_Object->addProperty("name", "Debug", "", SettingsNoValue);
	scummvm_Debug_Object->properties["buildSettings"] = scummvm_Debug;

	// Release
	Object *scummvm_Release_Object = new Object(this, "XCBuildConfiguration_scummvm_Release", "scummvm", "XCBuildConfiguration", "PBXProject", "Release");
	Property scummvm_Release(scummvm_Debug);
	REMOVE_SETTING(scummvm_Release, "GCC_C_LANGUAGE_STANDARD");       // Not sure why we remove that, or any of the other warnings
	REMOVE_SETTING(scummvm_Release, "GCC_WARN_ABOUT_RETURN_TYPE");
	REMOVE_SETTING(scummvm_Release, "GCC_WARN_UNUSED_VARIABLE");
	REMOVE_SETTING(scummvm_Release, "ONLY_ACTIVE_ARCH");

	scummvm_Release_Object->addProperty("name", "Release", "", SettingsNoValue);
	scummvm_Release_Object->properties["buildSettings"] = scummvm_Release;

	_buildConfiguration.add(scummvm_Debug_Object);
	_buildConfiguration.add(scummvm_Release_Object);

	/****************************************
	 * ScummVM-OS X
	 ****************************************/

	// Debug
	Object *scummvmOSX_Debug_Object = new Object(this, "XCBuildConfiguration_ScummVM-OSX_Debug", _targets[1] /* ScummVM-OS X */, "XCBuildConfiguration", "PBXNativeTarget", "Debug");
	Property scummvmOSX_Debug;
	ADD_SETTING_QUOTE(scummvmOSX_Debug, "ARCHS", "$(NATIVE_ARCH)");
	ADD_SETTING(scummvmOSX_Debug, "COMPRESS_PNG_FILES", "NO");
	ADD_SETTING(scummvmOSX_Debug, "COPY_PHASE_STRIP", "NO");
	ADD_SETTING_QUOTE(scummvmOSX_Debug, "DEBUG_INFORMATION_FORMAT", "dwarf-with-dsym");
	ADD_SETTING_QUOTE(scummvmOSX_Debug, "FRAMEWORK_SEARCH_PATHS", "");
	ADD_SETTING(scummvmOSX_Debug, "GCC_C_LANGUAGE_STANDARD", "c99");
	ADD_SETTING(scummvmOSX_Debug, "GCC_ENABLE_CPP_EXCEPTIONS", "NO");
	ADD_SETTING(scummvmOSX_Debug, "GCC_ENABLE_CPP_RTTI", "NO");
	ADD_SETTING(scummvmOSX_Debug, "GCC_DYNAMIC_NO_PIC", "NO");
	ADD_SETTING(scummvmOSX_Debug, "GCC_ENABLE_FIX_AND_CONTINUE", "NO");
	ADD_SETTING(scummvmOSX_Debug, "GCC_OPTIMIZATION_LEVEL", "0");
	ADD_SETTING(scummvmOSX_Debug, "GCC_PRECOMPILE_PREFIX_HEADER", "NO");
	ADD_SETTING_QUOTE(scummvmOSX_Debug, "GCC_PREFIX_HEADER", "");
	ValueList scummvmOSX_defines(_defines);
	ADD_DEFINE(scummvmOSX_defines, "SDL_BACKEND");
	ADD_DEFINE(scummvmOSX_defines, "MACOSX");
	ADD_SETTING_LIST(scummvmOSX_Debug, "GCC_PREPROCESSOR_DEFINITIONS", scummvmOSX_defines, SettingsNoQuote|SettingsAsList, 5);
	ADD_SETTING_QUOTE(scummvmOSX_Debug, "GCC_VERSION", "");
	ValueList scummvmOSX_HeaderPaths;
	SET_VALUES(scummvmOSX_HeaderPaths, "/opt/local/include/SDL", "/opt/local/include", "include/", "../../engines/", "../../");
	ADD_SETTING_LIST(scummvmOSX_Debug, "HEADER_SEARCH_PATHS", scummvmOSX_HeaderPaths, SettingsNoQuote|SettingsAsList, 5);
	ADD_SETTING_QUOTE(scummvmOSX_Debug, "INFOPLIST_FILE", "$(SRCROOT)/../macosx/Info.plist");
	ValueList scummvmOSX_LibPaths;
	SET_VALUES(scummvmOSX_LibPaths, "/sw/lib", "/opt/local/lib", "\"$(inherited)\"", "\"\\\\\\\"$(SRCROOT)/lib\\\\\\\"\""); // mmmh, all those slashes, it's almost Christmas \o/
	ADD_SETTING_LIST(scummvmOSX_Debug, "LIBRARY_SEARCH_PATHS", scummvmOSX_LibPaths, SettingsNoQuote|SettingsAsList, 5);
	ADD_SETTING_QUOTE(scummvmOSX_Debug, "OTHER_CFLAGS", "");
	ValueList scummvmOSX_LdFlags;
	SET_VALUES(scummvmOSX_LdFlags, "-lSDLmain", "-logg", "-lvorbisfile", "-lvorbis", "-lmpeg2", "-lmad", "-lFLAC", "-lSDL", "-lz");
	ADD_SETTING_LIST(scummvmOSX_Debug, "OTHER_LDFLAGS", scummvmOSX_LdFlags, SettingsAsList, 5);
	ADD_SETTING(scummvmOSX_Debug, "PREBINDING", "NO");
	ADD_SETTING(scummvmOSX_Debug, "PRODUCT_NAME", "ScummVM");

	scummvmOSX_Debug_Object->addProperty("name", "Debug", "", SettingsNoValue);
	scummvmOSX_Debug_Object->properties["buildSettings"] = scummvmOSX_Debug;

	// Release
	Object *scummvmOSX_Release_Object = new Object(this, "XCBuildConfiguration_ScummVMOSX_Release", _targets[1] /* ScummVM-OS X */, "XCBuildConfiguration", "PBXNativeTarget", "Release");
	Property scummvmOSX_Release(scummvmOSX_Debug);
	ADD_SETTING(scummvmOSX_Release, "COPY_PHASE_STRIP", "YES");
	REMOVE_SETTING(scummvmOSX_Release, "GCC_DYNAMIC_NO_PIC");
	REMOVE_SETTING(scummvmOSX_Release, "GCC_OPTIMIZATION_LEVEL");
	ADD_SETTING(scummvmOSX_Release, "WRAPPER_EXTENSION", "app");

	scummvmOSX_Release_Object->addProperty("name", "Release", "", SettingsNoValue);
	scummvmOSX_Release_Object->properties["buildSettings"] = scummvmOSX_Release;

	_buildConfiguration.add(scummvmOSX_Debug_Object);
	_buildConfiguration.add(scummvmOSX_Release_Object);

	/****************************************
	 * ScummVM-Simulator
	 ****************************************/

	// Debug
	Object *scummvmSimulator_Debug_Object = new Object(this, "XCBuildConfiguration_ScummVM-Simulator_Debug", _targets[2] /* ScummVM-Simulator */, "XCBuildConfiguration", "PBXNativeTarget", "Debug");
	Property scummvmSimulator_Debug(iPhone_Debug);
	ADD_SETTING_QUOTE(scummvmSimulator_Debug, "FRAMEWORK_SEARCH_PATHS", "$(inherited)");
	ADD_SETTING_LIST(scummvmSimulator_Debug, "GCC_PREPROCESSOR_DEFINITIONS", scummvm_defines, SettingsNoQuote|SettingsAsList, 5);
	ADD_SETTING(scummvmSimulator_Debug, "SDKROOT", "iphonesimulator3.2");
	REMOVE_SETTING(scummvmSimulator_Debug, "TARGETED_DEVICE_FAMILY");

	scummvmSimulator_Debug_Object->addProperty("name", "Debug", "", SettingsNoValue);
	scummvmSimulator_Debug_Object->properties["buildSettings"] = scummvmSimulator_Debug;

	// Release
	Object *scummvmSimulator_Release_Object = new Object(this, "XCBuildConfiguration_ScummVM-Simulator_Release", _targets[2] /* ScummVM-Simulator */, "XCBuildConfiguration", "PBXNativeTarget", "Release");
	Property scummvmSimulator_Release(scummvmSimulator_Debug);
	ADD_SETTING(scummvmSimulator_Release, "COPY_PHASE_STRIP", "YES");
	REMOVE_SETTING(scummvmSimulator_Release, "GCC_DYNAMIC_NO_PIC");
	ADD_SETTING(scummvmSimulator_Release, "WRAPPER_EXTENSION", "app");

	scummvmSimulator_Release_Object->addProperty("name", "Release", "", SettingsNoValue);
	scummvmSimulator_Release_Object->properties["buildSettings"] = scummvmSimulator_Release;

	_buildConfiguration.add(scummvmSimulator_Debug_Object);
	_buildConfiguration.add(scummvmSimulator_Release_Object);

	//////////////////////////////////////////////////////////////////////////
	// Configuration List
	_configurationList.comment = "XCConfigurationList";
	_configurationList.flags = SettingsAsList;

	// Warning: This assumes we have all configurations with a Debug & Release pair
	for (std::vector<Object *>::iterator config = _buildConfiguration.objects.begin(); config != _buildConfiguration.objects.end(); config++) {

		Object *configList = new Object(this, "XCConfigurationList_" + (*config)->name, (*config)->name, "XCConfigurationList", "", "Build configuration list for " +  (*config)->refType + " \"" + (*config)->name + "\"");

		Property buildConfigs;
		buildConfigs.flags = SettingsAsList;

		buildConfigs.settings[getHash((*config)->id)] = Setting("", "Debug", SettingsNoValue, 0, 0);
		buildConfigs.settings[getHash((*(config++))->id)] = Setting("", "Release", SettingsNoValue, 0, 1);

		configList->properties["buildConfigurations"] = buildConfigs;

		configList->addProperty("defaultConfigurationIsVisible", "0", "", SettingsNoValue);
		configList->addProperty("defaultConfigurationName", "Release", "", SettingsNoValue);

		_configurationList.add(configList);
	}
}

//////////////////////////////////////////////////////////////////////////
// Misc
//////////////////////////////////////////////////////////////////////////

// Setup global defines
void XCodeProvider::setupDefines(const BuildSetup &setup) {

	for (StringList::const_iterator i = setup.defines.begin(); i != setup.defines.end(); ++i) {
		if (*i == "HAVE_NASM")	// Not supported on Mac (TODO: change how it's handled in main class or add it only in MSVC/CodeBlocks providers?)
			continue;

		ADD_DEFINE(_defines, *i);
	}
	// Add special defines for Mac support
	ADD_DEFINE(_defines, "CONFIG_H");
	ADD_DEFINE(_defines, "SCUMM_NEED_ALIGNMENT");
	ADD_DEFINE(_defines, "SCUMM_LITTLE_ENDIAN");
	ADD_DEFINE(_defines, "UNIX");
	ADD_DEFINE(_defines, "SCUMMVM");
	ADD_DEFINE(_defines, "USE_TREMOR");
}

//////////////////////////////////////////////////////////////////////////
// Object hash
//////////////////////////////////////////////////////////////////////////

// TODO use md5 to compute a file hash (and fall back to standard key generation if not passed a file)
std::string XCodeProvider::getHash(std::string key) {

#if DEBUG_XCODE_HASH
	return key;
#else
	// Check to see if the key is already in the dictionary
	std::map<std::string, std::string>::iterator hashIterator = _hashDictionnary.find(key);
	if (hashIterator != _hashDictionnary.end())
		return hashIterator->second;

	// Generate a new key from the file hash and insert it into the dictionary
	std::string hash = newHash();
	_hashDictionnary[key] = hash;

	return hash;
#endif
}

bool isSeparator (char s) { return (s == '-'); }

std::string XCodeProvider::newHash() const {
	std::string hash = createUUID();

	// Remove { and - from UUID and resize to 96-bits uppercase hex string
	hash.erase(remove_if(hash.begin(), hash.end(), isSeparator), hash.end());

	hash.resize(24);
	std::transform(hash.begin(), hash.end(), hash.begin(), toupper);

	return hash;
}

//////////////////////////////////////////////////////////////////////////
// Output
//////////////////////////////////////////////////////////////////////////

std::string replace(std::string input, const std::string find, std::string replaceStr) {
	std::string::size_type pos = 0;
	std::string::size_type findLen = find.length();
	std::string::size_type replaceLen = replaceStr.length();

	if (findLen == 0 )
		return input;

	for (;(pos = input.find(find, pos)) != std::string::npos;) {
		input.replace(pos, findLen, replaceStr);
		pos += replaceLen;
	}

	return input;
}

std::string XCodeProvider::writeProperty(const std::string &variable, Property &prop, int flags) const {
	std::string output;

	output += (flags & SettingsSingleItem ? "" : "\t\t\t") + variable + " = ";

	if (prop.settings.size() > 1 || (prop.flags & SettingsSingleItem))
		output += (prop.flags & SettingsAsList) ? "(\n" : "{\n";

	OrderedSettingList settings = prop.getOrderedSettingList();
	for (OrderedSettingList::const_iterator setting = settings.begin(); setting != settings.end(); ++setting) {
		if (settings.size() > 1 || (prop.flags & SettingsSingleItem))
			output += (flags & SettingsSingleItem ? " " : "\t\t\t\t");

		output += writeSetting((*setting).first, (*setting).second);

		if ((prop.flags & SettingsAsList) && prop.settings.size() > 1) {
			output += (prop.settings.size() > 0) ? ",\n" : "\n";
		} else {
			output += ";";
			output += (flags & SettingsSingleItem ? " " : "\n");
		}
	}

	if (prop.settings.size() > 1 || (prop.flags & SettingsSingleItem))
		output += (prop.flags & SettingsAsList) ? "\t\t\t);\n" : "\t\t\t};\n";

	return output;
}

std::string XCodeProvider::writeSetting(const std::string &variable, std::string value, std::string comment, int flags, int indent) const {
	return writeSetting(variable, Setting(value, comment, flags, indent));
}
// Heavily modified (not in a good way) function, imported from QMake XCode project generator (licensed under the QT license)
std::string XCodeProvider::writeSetting(const std::string &variable, const Setting &setting) const {
	std::string output;
	const std::string quote = (setting.flags & SettingsNoQuote) ? "" : "\"";
	const std::string escape_quote = quote.empty() ? "" : "\\" + quote;
	std::string newline = "\n";

	// Get indent level
	for (int i = 0; i < setting.indent; ++i)
		newline += "\t";

	// Setup variable
	std::string var = (setting.flags & SettingsQuoteVariable) ? "\"" + variable + "\"" : variable;

	// Output a list
	if (setting.flags & SettingsAsList) {

		output += var + ((setting.flags & SettingsNoValue) ? "(" : " = (") + newline;

		for (unsigned int i = 0, count = 0; i < setting.entries.size(); ++i) {

			std::string value = setting.entries.at(i).value;
			if(!value.empty()) {
				if (count++ > 0)
					output += "," + newline;

				output += quote + replace(value, quote, escape_quote) + quote;

				std::string comment = setting.entries.at(i).comment;
				if (!comment.empty())
					output += " /* " + comment + " */";
			}

		}
		// Add closing ")" on new line
		newline.resize(newline.size() - 1);
		output += (setting.flags & SettingsNoValue) ? "\t\t\t)" : "," + newline + ")";
	} else {
		output += var;

		output += (setting.flags & SettingsNoValue) ? "" : " = " + quote;

		for(unsigned int i = 0; i < setting.entries.size(); ++i) {
			std::string value = setting.entries.at(i).value;
			if(i)
				output += " ";
			output += value;

			std::string comment = setting.entries.at(i).comment;
			if (!comment.empty())
				output += " /* " + comment + " */";
		}

		output += (setting.flags & SettingsNoValue) ? "" : quote;
	}
	return output;
}

} // End of CreateProjectTool namespace
