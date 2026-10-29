#include <cmath>
#include "rhi/webgpu/WebGpuInternal.hpp"

namespace rhi::webgpu {
    namespace {
        WGPUPrimitiveTopology ToTopology(Topology t) {
            switch (t) {
            case Topology::TriangleList: return WGPUPrimitiveTopology_TriangleList;
            case Topology::TriangleStrip: return WGPUPrimitiveTopology_TriangleStrip;
            case Topology::LineList: return WGPUPrimitiveTopology_LineList;
            case Topology::PointList: return WGPUPrimitiveTopology_PointList;
            }
            return WGPUPrimitiveTopology_TriangleList;
        }

        WGPUBlendComponent Component(BlendOp op, BlendFactor src, BlendFactor dst) {
            WGPUBlendComponent c;
            c.operation = ToBlendOp(op);
            c.srcFactor = ToBlendFactor(src);
            c.dstFactor = ToBlendFactor(dst);
            // min and max ignore the factors, and webgpu insists they are one
            if (op == BlendOp::Min || op == BlendOp::Max)
                c.srcFactor = c.dstFactor = WGPUBlendFactor_One;
            return c;
        }

        WGPUStencilFaceState Face(const StencilFace& f) {
            return {ToCompare(f.compare), ToStencilOp(f.fail), ToStencilOp(f.depthFail), ToStencilOp(f.pass)};
        }
    }

    PipelineHandle WebGpuDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
        const PipelineLayout* pl = pipelineLayouts.Get(desc.layout);
        const Shader* vs = shaders.Get(desc.vertex);
        const Shader* fs = shaders.Get(desc.fragment);
        if (!pl || !vs) {
            ReportError("CreateGraphicsPipeline: missing layout or vertex shader");
            return {};
        }
        Pipeline p;
        p.layoutHandle = desc.layout;
        std::array<WGPUBindGroupLayout, kMaxBindGroups> layouts{};
        const Shader* stages[2] = {vs, fs};
        uint32_t setCount = 0;
        for (uint32_t s = 0; s < kMaxBindGroups; s++) {
            if (!pl->desc.sets[s])
                continue;
            p.sets[s] = BuildNativeSet(pl->desc, s, stages, ShaderStage::AllGraphics);
            if (!p.sets[s])
                return {};
            setCount = s + 1;
        }
        for (uint32_t s = 0; s < setCount; s++)
            layouts[s] = p.sets[s] ? p.sets[s]->layout.Get() : emptyLayout.Get();
        WGPUPipelineLayoutDescriptor pld{};
        pld.label = View(desc.name);
        pld.bindGroupLayoutCount = setCount;
        pld.bindGroupLayouts = layouts.data();
        p.layout = Owned<WGPUPipelineLayout>(wgpuDeviceCreatePipelineLayout(device.Get(), &pld));
        if (!p.layout)
            return {};

        std::array<std::vector<WGPUVertexAttribute>, kMaxVertexStreams> attributes;
        for (uint32_t i = 0; i < desc.attributeCount; i++) {
            const VertexAttributeDesc& a = desc.attributes[i];
            const WGPUVertexFormat vf = ToVertexFormat(a.format);
            if (vf == static_cast<WGPUVertexFormat>(0) || a.stream >= desc.streamCount) {
                ReportError("CreateGraphicsPipeline: unusable vertex attribute");
                return {};
            }
            attributes[a.stream].push_back({vf, a.offset, a.location});
        }
        std::array<WGPUVertexBufferLayout, kMaxVertexStreams> streams{};
        for (uint32_t i = 0; i < desc.streamCount; i++) {
            streams[i].stepMode = desc.streams[i].step == VertexStep::Instance ? WGPUVertexStepMode_Instance : WGPUVertexStepMode_Vertex;
            streams[i].arrayStride = desc.streams[i].stride;
            streams[i].attributeCount = attributes[i].size();
            streams[i].attributes = attributes[i].data();
        }
        p.streamCount = desc.streamCount;
        p.setCount = setCount;

        WGPURenderPipelineDescriptor rd{};
        rd.label = View(desc.name);
        rd.layout = p.layout.Get();
        rd.vertex.module = vs->module.Get();
        rd.vertex.entryPoint = View(vs->entry);
        rd.vertex.bufferCount = desc.streamCount;
        rd.vertex.buffers = streams.data();

        rd.primitive.topology = ToTopology(desc.topology);
        rd.primitive.frontFace = desc.raster.frontFace == FrontFace::Clockwise ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
        rd.primitive.cullMode = desc.raster.cull == CullMode::None ? WGPUCullMode_None
                                : desc.raster.cull == CullMode::Back ? WGPUCullMode_Back
                                                                      : WGPUCullMode_Front;
        rd.primitive.unclippedDepth = !desc.raster.depthClip && caps.features.depthClipControl;

        WGPUDepthStencilState ds{};
        if (desc.depthFormat != Format::Unknown) {
            ds.format = ToTextureFormat(desc.depthFormat);
            ds.depthWriteEnabled = desc.depthStencil.depthWrite ? WGPUOptionalBool_True : WGPUOptionalBool_False;
            ds.depthCompare = desc.depthStencil.depthTest ? ToCompare(desc.depthStencil.depthCompare) : WGPUCompareFunction_Always;
            if (desc.depthStencil.stencilTest) {
                ds.stencilFront = Face(desc.depthStencil.front);
                ds.stencilBack = Face(desc.depthStencil.back);
            } else {
                ds.stencilFront = ds.stencilBack = {WGPUCompareFunction_Always, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep,
                                                    WGPUStencilOperation_Keep};
            }
            ds.stencilReadMask = desc.depthStencil.stencilReadMask;
            ds.stencilWriteMask = desc.depthStencil.stencilTest ? desc.depthStencil.stencilWriteMask : 0;
            ds.depthBias = static_cast<int32_t>(std::lround(desc.raster.depthBiasConstant));
            ds.depthBiasSlopeScale = desc.raster.depthBiasSlope;
            ds.depthBiasClamp = desc.raster.depthBiasClamp;
            rd.depthStencil = &ds;
        }

        rd.multisample.count = (std::max)(1u, desc.samples);
        rd.multisample.mask = 0xFFFFFFFFu;
        rd.multisample.alphaToCoverageEnabled = desc.blend.alphaToCoverage && rd.multisample.count > 1;

        std::array<WGPUColorTargetState, kMaxColorTargets> targets{};
        std::array<WGPUBlendState, kMaxColorTargets> blends{};
        WGPUFragmentState fragment{};
        if (fs) {
            for (uint32_t i = 0; i < desc.colorCount; i++) {
                const BlendTarget& bt = desc.blend.targets[i];
                targets[i].format = ToTextureFormat(desc.colorFormats[i]);
                targets[i].writeMask = static_cast<WGPUColorWriteMask>(bt.writeMask);
                if (bt.enable) {
                    blends[i].color = Component(bt.colorOp, bt.srcColor, bt.dstColor);
                    blends[i].alpha = Component(bt.alphaOp, bt.srcAlpha, bt.dstAlpha);
                    targets[i].blend = &blends[i];
                }
            }
            fragment.module = fs->module.Get();
            fragment.entryPoint = View(fs->entry);
            fragment.targetCount = desc.colorCount;
            fragment.targets = targets.data();
            rd.fragment = &fragment;
        }

        p.render = Owned<WGPURenderPipeline>(wgpuDeviceCreateRenderPipeline(device.Get(), &rd));
        if (!p.render)
            return {};
        stats.livePipelines++;
        return pipelines.Alloc(std::move(p));
    }

    PipelineHandle WebGpuDevice::CreateComputePipeline(const ComputePipelineDesc& desc) {
        const PipelineLayout* pl = pipelineLayouts.Get(desc.layout);
        const Shader* cs = shaders.Get(desc.compute);
        if (!pl || !cs) {
            ReportError("CreateComputePipeline: missing layout or shader");
            return {};
        }
        Pipeline p;
        p.compute = true;
        p.layoutHandle = desc.layout;
        std::array<WGPUBindGroupLayout, kMaxBindGroups> layouts{};
        const Shader* stages[1] = {cs};
        uint32_t setCount = 0;
        for (uint32_t s = 0; s < kMaxBindGroups; s++) {
            if (!pl->desc.sets[s])
                continue;
            p.sets[s] = BuildNativeSet(pl->desc, s, stages, ShaderStage::Compute);
            if (!p.sets[s])
                return {};
            setCount = s + 1;
        }
        for (uint32_t s = 0; s < setCount; s++)
            layouts[s] = p.sets[s] ? p.sets[s]->layout.Get() : emptyLayout.Get();
        WGPUPipelineLayoutDescriptor pld{};
        pld.label = View(desc.name);
        pld.bindGroupLayoutCount = setCount;
        pld.bindGroupLayouts = layouts.data();
        p.layout = Owned<WGPUPipelineLayout>(wgpuDeviceCreatePipelineLayout(device.Get(), &pld));
        if (!p.layout)
            return {};
        WGPUComputePipelineDescriptor cd{};
        cd.label = View(desc.name);
        cd.layout = p.layout.Get();
        cd.compute.module = cs->module.Get();
        cd.compute.entryPoint = View(cs->entry);
        p.setCount = setCount;
        p.comp = Owned<WGPUComputePipeline>(wgpuDeviceCreateComputePipeline(device.Get(), &cd));
        if (!p.comp)
            return {};
        stats.livePipelines++;
        return pipelines.Alloc(std::move(p));
    }
}
