// Copyright 2019 Luma Pictures
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
//
// Modifications Copyright 2019 Autodesk, Inc.
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
#include "renderer_plugin.h"

#include <pxr/imaging/hd/rendererPluginRegistry.h>

#include "render_delegate.h"

#include <constant_strings.h>

PXR_NAMESPACE_OPEN_SCOPE

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
     ((houdini_renderer, "houdini:renderer")));
// clang-format on

// Register the Ai plugin with the renderer plugin system.
TF_REGISTRY_FUNCTION(TfType) { HdRendererPluginRegistry::Define<HdArnoldRendererPlugin>(); }

HdRenderDelegate* HdArnoldRendererPlugin::CreateRenderDelegate() { return new HdArnoldRenderDelegate(); }

HdRenderDelegate* HdArnoldRendererPlugin::CreateRenderDelegate(const HdRenderSettingsMap& settingsMap)
{
    auto context = HdArnoldRenderContext::Hydra;
    const auto* houdiniRenderer = TfMapLookupPtr(settingsMap, _tokens->houdini_renderer);
    if (houdiniRenderer != nullptr &&
        ((houdiniRenderer->IsHolding<TfToken>() && houdiniRenderer->UncheckedGet<TfToken>() == str::t_husk) ||
         (houdiniRenderer->IsHolding<std::string>() &&
          houdiniRenderer->UncheckedGet<std::string>() == str::t_husk.GetString()))) {
        context = HdArnoldRenderContext::Husk;
    }
    auto* delegate = new HdArnoldRenderDelegate(context);
    for (const auto& setting : settingsMap) {
        delegate->SetRenderSetting(setting.first, setting.second);
    }
    return delegate;
}

void HdArnoldRendererPlugin::DeleteRenderDelegate(HdRenderDelegate* renderDelegate) { delete renderDelegate; }

bool HdArnoldRendererPlugin::IsSupported() const { return true; }

PXR_NAMESPACE_CLOSE_SCOPE
