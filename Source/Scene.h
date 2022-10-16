// Copyright (c) 2020-2021 Sultim Tsyrendashiev
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include "ASManager.h"
#include "LightManager.h"
#include "VertexPreprocessing.h"

namespace RTGL1
{

class Scene
{
public:
    explicit Scene( VkDevice                                device,
                    const PhysicalDevice&                   physDevice,
                    std::shared_ptr< MemoryAllocator >&     allocator,
                    std::shared_ptr< CommandBufferManager > cmdManager,
                    std::shared_ptr< TextureManager >       textureManager,
                    const GlobalUniform&                    uniform,
                    const ShaderManager&                    shaderManager );
    ~Scene() = default;

    Scene( const Scene& other )     = delete;
    Scene( Scene&& other ) noexcept = delete;
    Scene& operator=( const Scene& other ) = delete;
    Scene& operator=( Scene&& other ) noexcept = delete;

    void   PrepareForFrame( VkCommandBuffer cmd, uint32_t frameIndex );
    void   SubmitForFrame( VkCommandBuffer                         cmd,
                           uint32_t                                frameIndex,
                           const std::shared_ptr< GlobalUniform >& uniform,
                           uint32_t                                uniformData_rayCullMaskWorld,
                           bool                                    allowGeometryWithSkyFlag,
                           bool                                    disableRTGeometry );

    bool   Upload( uint32_t                   frameIndex,
                   const RgMeshInfo&          mesh,
                   const RgMeshPrimitiveInfo& primitive );
    // bool UpdateTransform(const RgUpdateTransformInfo &updateInfo);
    // bool UpdateTexCoords(const RgUpdateTexCoordsInfo &texCoordsInfo);

    void   UploadLight( uint32_t frameIndex, const RgSphericalLightUploadInfo& lightInfo );
    void   UploadLight( uint32_t frameIndex, const RgPolygonalLightUploadInfo& lightInfo );
    void   UploadLight( uint32_t frameIndex, const RgDirectionalLightUploadInfo& lightInfo );
    void   UploadLight( uint32_t frameIndex, const RgSpotLightUploadInfo& lightInfo );

    void   SubmitStatic();
    void   StartNewStatic();

    const std::shared_ptr< ASManager >&           GetASManager();
    const std::shared_ptr< LightManager >&        GetLightManager();
    const std::shared_ptr< VertexPreprocessing >& GetVertexPreprocessing();

    bool                                          DoesUniqueIDExist( uint64_t uniqueID ) const;

private:
    bool TryGetStaticSimpleIndex( uint64_t uniqueID, uint32_t* result ) const;

private:
    std::shared_ptr< ASManager >             asManager;
    std::shared_ptr< LightManager >          lightManager;
    std::shared_ptr< GeomInfoManager >       geomInfoMgr;
    std::shared_ptr< VertexPreprocessing >   vertPreproc;

    // Dynamic indices are cleared every frame
    rgl::unordered_map< uint64_t, uint32_t > dynamicUniqueIDToSimpleIndex;
    rgl::unordered_map< uint64_t, uint32_t > staticUniqueIDToSimpleIndex;

    // Movable geometry IDs
    std::vector< uint32_t >                  movableGeomIndices;
    bool                                     toResubmitMovable;

    bool                                     isRecordingStatic;
    bool                                     submittedStaticInCurrentFrame;
};

}