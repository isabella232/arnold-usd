// Copyright 2019 Autodesk, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <ai.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../utils/utils.h"
#include "reader.h"
#include "registry.h"
#include <constant_strings.h>
#include <pxr/base/tf/pathUtils.h>

#if defined(_DARWIN) || defined(_LINUX)
#include <dlfcn.h>
#endif

// Macro magic to expand the procedural name.
#define XARNOLDUSDSTRINGIZE(x) ARNOLDUSDSTRINGIZE(x)
#define ARNOLDUSDSTRINGIZE(x) #x

//-*************************************************************************
// Code for the Arnold procedural node loading USD files

AI_PROCEDURAL_NODE_EXPORT_METHODS(UsdProceduralMethods);

node_parameters
{
    AiParameterStr("filename", "");
    AiParameterStr("object_path", "");
    AiParameterFlt("frame", 0.0);
    AiParameterBool("debug", false);
    AiParameterInt("threads", 0);
    AiParameterArray("overrides", AiArray(0, 1, AI_TYPE_STRING));
    AiParameterInt("cache_id", 0);
    
    // Set metadata that triggers the re-generation of the procedural contents when this attribute
    // is modified (see #176)
    AiMetaDataSetBool(nentry, AtString("filename"), AtString("_triggers_reload"), true);
    AiMetaDataSetBool(nentry, AtString("object_path"), AtString("_triggers_reload"), true);
    AiMetaDataSetBool(nentry, AtString("frame"), AtString("_triggers_reload"), true);
    AiMetaDataSetBool(nentry, AtString("overrides"), AtString("_triggers_reload"), true);
    AiMetaDataSetBool(nentry, AtString("cache_id"), AtString("_triggers_reload"), true);

    // This type of procedural can be initialized in parallel
    AiMetaDataSetBool(nentry, AtString(""), AtString("parallel_init"), true);
}
typedef std::vector<std::string> PathList;

void applyProceduralSearchPath(std::string &filename, const AtUniverse *universe)
{
    AtNode *optionsNode = AiUniverseGetOptions(universe);
    if (optionsNode) {
        // We want to allow using the procedural search path to point to directories containing .abc files in the same
        // way procedural search paths are used to resolve procedural .ass files.
        // To do this we extract the procedural path from the options node, where environment variables specified using
        // the Arnold standard (e.g. [HOME]) are expanded. If our .abc file exists in any of the directories we
        // concatenate the path and the relative filename to create a new procedural argument filename using the full
        // path.
        std::string proceduralPath = std::string(AiNodeGetStr(optionsNode, "procedural_searchpath"));
        std::string expandedSearchpath = ExpandEnvironmentVariables(proceduralPath.c_str());

        PathList pathList;
        TokenizePath(expandedSearchpath, pathList, ":;", true);
        if (!pathList.empty()) {
            for (PathList::const_iterator it = pathList.begin(); it != pathList.end(); ++it) {
                std::string path = *it;
                std::string fullPath = PathJoin(path.c_str(), filename.c_str());
                if (IsFileAccessible(fullPath)) {
                    filename = fullPath;
                    return;
                }
            }
        }
    }
}

procedural_init
{
    UsdArnoldReader *data = new UsdArnoldReader();
    *user_ptr = data;

    std::string objectPath(AiNodeGetStr(node, "object_path"));
    data->SetProceduralParent(node);
    data->SetFrame(AiNodeGetFlt(node, "frame"));
    data->SetDebug(AiNodeGetBool(node, "debug"));
    data->SetThreadCount(AiNodeGetInt(node, "threads"));

    AtNode *renderCam = AiUniverseGetCamera();
    if (renderCam &&
        (AiNodeGetFlt(renderCam, AtString("shutter_start")) < AiNodeGetFlt(renderCam, AtString("shutter_end")))) {
        float motionStart = AiNodeGetFlt(renderCam, AtString("shutter_start"));
        float motionEnd = AiNodeGetFlt(renderCam, AtString("shutter_end"));
        data->SetMotionBlur((motionStart < motionEnd), motionStart, motionEnd);
    } else {
        data->SetMotionBlur(false);
    }

    int cache_id = AiNodeGetInt(node, "cache_id");
    if (cache_id != 0) {
        // We have an id to load the Usd Stage in memory, using UsdStageCache
        data->Read(cache_id, objectPath);
    } else {
        // We load a usd file, with eventual serialized overrides
        std::string filename(AiNodeGetStr(node, "filename"));
        applyProceduralSearchPath(filename, nullptr);
        data->Read(filename, AiNodeGetArray(node, "overrides"), objectPath);
    }
    return 1;
}

//-*************************************************************************

procedural_cleanup
{
    delete reinterpret_cast<UsdArnoldReader *>(user_ptr);
    return 1;
}

//-*************************************************************************

procedural_num_nodes
{
    UsdArnoldReader *data = reinterpret_cast<UsdArnoldReader *>(user_ptr);
    if (data) {
        return data->GetNodes().size();
    }
    return 0;
}

//-*************************************************************************

procedural_get_node
{
    UsdArnoldReader *data = reinterpret_cast<UsdArnoldReader *>(user_ptr);
    if (data) {
        return data->GetNodes()[i];
    }
    return NULL;
}

#if AI_VERSION_ARCH_NUM >= 6
// New API function introduced in Arnold 6 for viewport display of procedurals
//
// ProceduralViewport(const AtNode* node,
//                    AtUniverse* universe,
//                    AtProcViewportMode mode, (AI_PROC_BOXES = 0, AI_PROC_POINTS, AI_PROC_POLYGONS)
//                    AtParamValueMap* params)
procedural_viewport
{
    int cache_id = AiNodeGetInt(node, "cache_id");

    std::string filename(AiNodeGetStr(node, "filename"));
    AtArray *overrides = AiNodeGetArray(node, "overrides");

    // We support empty filenames if overrides are being set #552
    bool hasOverrides = (overrides &&  AiArrayGetNumElements(overrides) > 0);
    if (cache_id == 0) {
        if (filename.empty()) {
            if (!hasOverrides)
                return false; // no filename + no override, nothing to show here
        } else {
            applyProceduralSearchPath(filename, universe);
            if (!UsdStage::IsSupportedFile(filename)) {
                AiMsgError("[usd] File not supported : %s", filename.c_str());
                return false;
            }
        }
    }

    // For now we always create a new reader for the viewport display,
    // can we reuse the eventual existing one ?
    UsdArnoldReader *reader = new UsdArnoldReader();

    std::string objectPath(AiNodeGetStr(node, "object_path"));
    // note that we must *not* set the parent procedural, as we'll be creating
    // nodes in a separate universe
    reader->SetFrame(AiNodeGetFlt(node, "frame"));
    reader->SetUniverse(universe);
    reader->SetThreadCount(AiNodeGetInt(node, "threads"));

    UsdArnoldViewportReaderRegistry *vpRegistry = nullptr;
    bool listNodes = false;
    // If we receive the bool param value "list" set to true, then we're being
    // asked to return the list of nodes in the usd file. We just need to create
    // the AtNodes, but not to convert them
    if (params && AiParamValueMapGetBool(params, AtString("list"), &listNodes) && listNodes) {
        reader->SetConvertPrimitives(false);
    } else {
        // We want a viewport reader registry, that will load either boxes, points or polygons
        vpRegistry = new UsdArnoldViewportReaderRegistry(mode, params);
        reader->SetRegistry(vpRegistry);
        // We want to read the "proxy" purpose
        reader->SetPurpose("proxy"); 
    }

    if (cache_id != 0) 
        reader->Read(cache_id, objectPath);
    else
        reader->Read(filename, overrides, objectPath);

    if (vpRegistry)
        delete vpRegistry;
    delete reader;
    return true;
}
#endif

#if defined(_DARWIN) || defined(_LINUX)
std::string USDLibraryPath()
{
    Dl_info info;
    if (dladdr("USDLibraryPath", &info)) {
        std::string path = info.dli_fname;
        return path;
    }

    return std::string();
}
#endif

node_loader
{
    if (i > 0) {
        return false;
    }

    node->methods = UsdProceduralMethods;
    node->output_type = AI_TYPE_NONE;
    node->name = AtString(XARNOLDUSDSTRINGIZE(USD_PROCEDURAL_NAME));
    node->node_type = AI_NODE_SHAPE_PROCEDURAL;
    strcpy(node->version, AI_VERSION);

    /* Fix the pre-10.13 OSX crashes at shutdown (#8866). Manually dlopening usd
     * prevents it from being unloaded since loads are reference counted
     * see : https://github.com/openssl/openssl/issues/653#issuecomment-206343347
     *       https://github.com/jemalloc/jemalloc/issues/1122
     */
#if defined(_DARWIN) || defined(_LINUX)
    const auto result = dlopen(USDLibraryPath().c_str(), RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE);
    if (!result)
        AiMsgWarning(
            "[USD] failed to re-load usd_proc.dylib. Crashes might happen on pre-10.13 OSX systems: %s\n", dlerror());
#endif
    return true;
}

/** Arnold 6.0.2.0 introduces Scene Format plugins.
 *  The following code is meant to add support for USD format,
 *  and kick directly USD files
 **/
#ifdef ARNOLD_HAS_SCENE_FORMAT_API
#include <ai_scene_format.h>

AI_SCENE_FORMAT_EXPORT_METHODS(UsdSceneFormatMtd);
#include "writer.h"

// SceneLoad(AtUniverse* universe, const char* filename, const AtParamValueMap* params)
scene_load
{
    if (!UsdStage::IsSupportedFile(filename)) {
        AiMsgError("[usd] File not supported : %s", filename);
        return false;
    }

    // Create a reader with no procedural parent
    UsdArnoldReader *reader = new UsdArnoldReader();
    // set the arnold universe on which the scene will be converted
    reader->SetUniverse(universe);
    // default to options.frame
    float frame = AiNodeGetFlt(AiUniverseGetOptions(), "frame");
    int threadCount = 0;
    
    if (params) {
        // eventually check the input param map in case we have an entry for "frame"
        AiParamValueMapGetFlt(params, AtString("frame"), &frame);
        // eventually get an amount of threads to read the usd file
        AiParamValueMapGetInt(params, AtString("threads"), &threadCount);
        int mask = AI_NODE_ALL;
        if (AiParamValueMapGetInt(params, AtString("mask"), &mask))
            reader->SetMask(mask);
    }
    reader->SetFrame(frame);
    reader->SetThreadCount(threadCount);

    // Read the USD file
    reader->Read(filename, nullptr);
    delete reader;
    return true;
}

// bool SceneWrite(AtUniverse* universe, const char* filename,
//                 const AtParamValueMap* params, const AtMetadataStore* mds)
scene_write
{
    std::string filenameStr(filename);
    if (!UsdStage::IsSupportedFile(filenameStr)) {
        // This filename isn't supported, let's see if it's just the extension that is upper-case
        std::string extension = TfGetExtension(filenameStr);
        size_t basenameLength = filenameStr.length() - extension.length();
        std::transform(
            filenameStr.begin() + basenameLength, filenameStr.end(), filenameStr.begin() + basenameLength, ::tolower);

        // Let's try again now, with a lower case extension
        if (UsdStage::IsSupportedFile(filenameStr)) {
            AiMsgWarning("[usd] File extension must be lower case. Saving as %s", filenameStr.c_str());
        } else {
            // Still not good, we cannot write to this file
            AiMsgError("[usd] File not supported : %s", filenameStr.c_str());
            return false;
        }
    }
    // Create a new USD stage to write out the .usd file
    UsdStageRefPtr stage = UsdStage::Open(SdfLayer::CreateNew(filenameStr.c_str()));

    if (stage == nullptr) {
        AiMsgError("[usd] Unable to create USD stage from %s", filenameStr.c_str());
        return false;
    }

    // Create a "writer" Translator that will handle the conversion
    UsdArnoldWriter *writer = new UsdArnoldWriter();
    writer->SetUsdStage(stage); // give it the output stage

    // Check if a mask has been set through the params map
    if (params) {
        int mask = AI_NODE_ALL;
        if (AiParamValueMapGetInt(params, str::mask, &mask))
            writer->SetMask(mask); // only write out this type or arnold nodes

        AtString scope;
        if (AiParamValueMapGetStr(params, str::scope, &scope))
            writer->SetScope(std::string(scope.c_str()));

        bool allAttributes;
        if (AiParamValueMapGetBool(params, str::all_attributes, &allAttributes))
            writer->SetWriteAllAttributes(allAttributes);
    }
    writer->Write(universe);       // convert this universe please
    stage->GetRootLayer()->Save(); // Ask USD to save out the file

    AiMsgInfo("[usd] Saved scene as %s", filenameStr.c_str());
    delete writer;
    return true;
}

scene_format_loader
{
    static const char *extensions[] = {".usd", ".usda", ".usdc", NULL};

    format->methods = UsdSceneFormatMtd;
    format->extensions = extensions;
    format->name = "USD";
    format->description = "Load and write USD files in Arnold";
    strcpy(format->version, AI_VERSION);
    return true;
}

#endif
