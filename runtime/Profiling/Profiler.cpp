﻿/*
Copyright(c) 2016-2023 Panos Karabelas

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

//= INCLUDES =========================
#include "pch.h"
#include "Profiler.h"
#include "../RHI/RHI_Device.h"
#include "../RHI/RHI_CommandList.h"
#include "../RHI/RHI_Implementation.h"
#include "../RHI/RHI_SwapChain.h"
#include "../Core/ThreadPool.h"
#include "../Rendering/Renderer.h"
#include "../Resource/ResourceCache.h"
//====================================

//= NAMESPACES =====
using namespace std;
//==================

namespace Spartan
{
    // metrics - rhi
    uint32_t Profiler::m_rhi_draw                       = 0;
    uint32_t Profiler::m_rhi_dispatch                   = 0;
    uint32_t Profiler::m_rhi_bindings_buffer_index      = 0;
    uint32_t Profiler::m_rhi_bindings_buffer_vertex     = 0;
    uint32_t Profiler::m_rhi_bindings_buffer_constant   = 0;
    uint32_t Profiler::m_rhi_bindings_buffer_structured = 0;
    uint32_t Profiler::m_rhi_bindings_sampler           = 0;
    uint32_t Profiler::m_rhi_bindings_texture_sampled   = 0;
    uint32_t Profiler::m_rhi_bindings_shader_vertex     = 0;
    uint32_t Profiler::m_rhi_bindings_shader_pixel      = 0;
    uint32_t Profiler::m_rhi_bindings_shader_compute    = 0;
    uint32_t Profiler::m_rhi_bindings_render_target     = 0;
    uint32_t Profiler::m_rhi_bindings_texture_storage   = 0;
    uint32_t Profiler::m_rhi_bindings_descriptor_set    = 0;
    uint32_t Profiler::m_rhi_bindings_pipeline          = 0;
    uint32_t Profiler::m_rhi_pipeline_barriers          = 0;
    uint32_t Profiler::m_rhi_timeblock_count            = 0;

    // metrics - renderer
    uint32_t Profiler::m_renderer_meshes_rendered = 0;

    // metrics - time
    float Profiler::m_time_frame_avg  = 0.0f;
    float Profiler::m_time_frame_min  = numeric_limits<float>::max();
    float Profiler::m_time_frame_max  = numeric_limits<float>::lowest();
    float Profiler::m_time_frame_last = 0.0f;
    float Profiler::m_time_cpu_avg    = 0.0f;
    float Profiler::m_time_cpu_min    = numeric_limits<float>::max();
    float Profiler::m_time_cpu_max    = numeric_limits<float>::lowest();
    float Profiler::m_time_cpu_last   = 0.0f;
    float Profiler::m_time_gpu_avg    = 0.0f;
    float Profiler::m_time_gpu_min    = numeric_limits<float>::max();
    float Profiler::m_time_gpu_max    = numeric_limits<float>::lowest();
    float Profiler::m_time_gpu_last   = 0.0f;

    // memory
    uint32_t Profiler::m_descriptor_set_count    = 0;
    uint32_t Profiler::m_descriptor_set_capacity = 0;

    ProfilerGranularity Profiler::m_granularity = ProfilerGranularity::Light;

    namespace
    {
        // profiling options
        const uint32_t initial_capacity = 256;
        bool profiling_enabled          = false;
        bool profile_cpu                = true;
        bool profile_gpu                = true;
        float profiling_interval_sec    = 0.25f;
        float time_since_profiling_sec  = profiling_interval_sec;

        const uint32_t frames_to_accumulate = static_cast<uint32_t>(7.5f / profiling_interval_sec);
        const float weight_delta            = 1.0f / static_cast<float>(frames_to_accumulate);
        const float weight_history          = (1.0f - weight_delta);

        // time blocks (double buffered)
        int m_time_block_index = -1;
        vector<TimeBlock> m_time_blocks_write;
        vector<TimeBlock> m_time_blocks_read;

        // fps
        float m_fps = 0.0f;

        // gpu
        string gpu_name               = "N/A";
        string gpu_driver             = "N/A";
        string gpu_api                = "N/A";
        uint32_t gpu_memory_available = 0;
        uint32_t gpu_memory_used      = 0;

        // stutter detection
        float stutter_delta_ms = 1.0f;
        bool is_stuttering_cpu = false;
        bool is_stuttering_gpu = false;

        // metric drawing
        ostringstream oss_metrics;
        string metrics_str;
        float metrics_time_since_last_update = profiling_interval_sec;

        // misc
        bool poll                 = false;
        bool increase_capacity    = false;
        bool allow_time_block_end = true;
    }
  
    void Profiler::Initialize()
    {
        m_time_blocks_read.reserve(initial_capacity);
        m_time_blocks_read.resize(initial_capacity);
        m_time_blocks_write.reserve(initial_capacity);
        m_time_blocks_write.resize(initial_capacity);

        profiling_enabled = RHI_Context::gpu_profiling;
    }

    void Profiler::Shutdown()
    {
        ClearRhiMetrics();
    }

    void Profiler::PreTick()
    {
        if (!profiling_enabled)
            return;

        if (increase_capacity)
        {
            SwapBuffers();

            // double the size
            const uint32_t size_old = static_cast<uint32_t>(m_time_blocks_write.size());
            const uint32_t size_new = size_old << 1;

            m_time_blocks_read.reserve(size_new);
            m_time_blocks_read.resize(size_new);
            m_time_blocks_write.reserve(size_new);
            m_time_blocks_write.resize(size_new);

            increase_capacity = false;
            poll              = true;

            SP_LOG_WARNING("Time block list has grown to %d. Consider making the default capacity as large by default, to avoid re-allocating.", size_new);
        }

        ClearRhiMetrics();
    }

    void Profiler::PostTick()
    {
        // compute timings
        {
            is_stuttering_cpu = m_time_cpu_last > (m_time_cpu_avg + stutter_delta_ms);
            is_stuttering_gpu = m_time_gpu_last > (m_time_gpu_avg + stutter_delta_ms);
            m_time_cpu_last   = 0.0f;
            m_time_gpu_last   = 0.0f;

            for (const TimeBlock& time_block : m_time_blocks_read)
            {
                if (!time_block.IsComplete())
                    continue;

                if (!time_block.GetParent() && time_block.GetType() == TimeBlockType::Cpu)
                {
                    m_time_cpu_last += time_block.GetDuration();
                }

                if (!time_block.GetParent() && time_block.GetType() == TimeBlockType::Gpu)
                {
                    m_time_gpu_last += time_block.GetDuration();
                }
            }

            // cpu
            m_time_cpu_avg = (m_time_cpu_avg * weight_history) + m_time_cpu_last * weight_delta;
            m_time_cpu_min = Math::Helper::Min(m_time_cpu_min, m_time_cpu_last);
            m_time_cpu_max = Math::Helper::Max(m_time_cpu_max, m_time_cpu_last);

            // gpu
            m_time_gpu_avg = (m_time_gpu_avg * weight_history) + m_time_gpu_last * weight_delta;
            m_time_gpu_min = Math::Helper::Min(m_time_gpu_min, m_time_gpu_last);
            m_time_gpu_max = Math::Helper::Max(m_time_gpu_max, m_time_gpu_last);

            // frame
            m_time_frame_last = static_cast<float>(Timer::GetDeltaTimeMs());
            m_time_frame_avg  = (m_time_frame_avg * weight_history) + m_time_frame_last * weight_delta;
            m_time_frame_min  = Math::Helper::Min(m_time_frame_min, m_time_frame_last);
            m_time_frame_max  = Math::Helper::Max(m_time_frame_max, m_time_frame_last);

            // fps  
            m_fps = 1000.0f / m_time_frame_avg;
        }

        // check whether we should profile or not
        time_since_profiling_sec += static_cast<float>(Timer::GetDeltaTimeSec());
        if (time_since_profiling_sec >= profiling_interval_sec)
        {
            time_since_profiling_sec = 0.0f;
            poll                     = true;
        }
        else if (poll)
        {
            poll = false;
        }

        if (poll && profiling_enabled)
        {
            AcquireGpuData();
            SwapBuffers();
        }

        if (Renderer::GetOption<bool>(Renderer_Option::Debug_PerformanceMetrics))
        {
            DrawPerformanceMetrics();
        }
    }

    void Profiler::SwapBuffers()
    {
        // copy completed time blocks write to time blocks read vector (double buffering)
        {
            uint32_t pass_index_gpu = 0;

            for (uint32_t i = 0; i < static_cast<uint32_t>(m_time_blocks_read.size()); i++)
            {
                TimeBlock& time_block = m_time_blocks_write[i];

                // compute time block duration
                if (time_block.IsComplete())
                {
                    if (time_block.GetType() == TimeBlockType::Gpu)
                    {
                        pass_index_gpu += 2;
                    }
                }
                else if (time_block.GetType() != TimeBlockType::Undefined) // if undefined, then it wasn't used this frame, nothing wrong with that.
                {
                    SP_LOG_WARNING("TimeBlockEnd() was not called for time block \"%s\"", time_block.GetName());
                }

                // copy over
                m_time_blocks_read[i] = time_block;

                // reset
                time_block.Reset();
            }
        }
        
        m_time_block_index = -1;
    }

    void Profiler::TimeBlockStart(const char* func_name, TimeBlockType type, RHI_CommandList* cmd_list /*= nullptr*/)
    {
        if (!profiling_enabled || !poll)
            return;

        const bool can_profile_cpu = (type == TimeBlockType::Cpu) && profile_cpu;
        const bool can_profile_gpu = (type == TimeBlockType::Gpu) && profile_gpu;

        if (!can_profile_cpu && !can_profile_gpu)
            return;

        // last incomplete block of the same type, is the parent
        TimeBlock* time_block_parent = GetLastIncompleteTimeBlock(type);

        if (TimeBlock* time_block = GetNewTimeBlock())
        {
            time_block->Begin(++m_rhi_timeblock_count, func_name, type, time_block_parent, cmd_list);
        }
    }

    void Profiler::TimeBlockEnd()
    {
        if (TimeBlock* time_block = GetLastIncompleteTimeBlock())
        {
            time_block->End();
        }
    }

    void Profiler::ClearMetrics()
    {
        m_time_frame_avg  = 0.0f;
        m_time_frame_min  = numeric_limits<float>::max();
        m_time_frame_max  = numeric_limits<float>::lowest();
        m_time_frame_last = 0.0f;
        m_time_cpu_avg    = 0.0f;
        m_time_cpu_min    = numeric_limits<float>::max();
        m_time_cpu_max    = numeric_limits<float>::lowest();
        m_time_cpu_last   = 0.0f;
        m_time_gpu_avg    = 0.0f;
        m_time_gpu_min    = numeric_limits<float>::max();
        m_time_gpu_max    = numeric_limits<float>::lowest();
        m_time_gpu_last   = 0.0f;
    }

    bool Profiler::GetEnabled()
    {
        return profiling_enabled;
    }

    void Profiler::SetEnabled(const bool enabled)
    {
        profiling_enabled = enabled;
    }

    const vector<TimeBlock>& Profiler::GetTimeBlocks()
    {
        return m_time_blocks_read;
    }

    float Profiler::GetTimeCpuLast()
    {
        return m_time_cpu_last;
    }

    float Profiler::GetTimeGpuLast()
    {
        return m_time_gpu_last;
    }

    float Profiler::GetTimeFrameLast()
    {
        return m_time_frame_last;
    }

    float Profiler::GetFps()
    {
        return m_fps;
    }

    float Profiler::GetUpdateInterval()
    {
        return profiling_interval_sec;
    }

    void Profiler::SetUpdateInterval(float interval)
    {
        profiling_interval_sec = interval;
    }

    const string& Profiler::GpuGetName()
    {
        return gpu_name;
    }

    uint32_t Profiler::GpuGetMemoryAvailable()
    {
        return gpu_memory_available;
    }

    uint32_t Profiler::GpuGetMemoryUsed()
    {
        return gpu_memory_used;
    }

    bool Profiler::IsCpuStuttering()
    {
        return is_stuttering_cpu;
    }

    bool Profiler::IsGpuStuttering()
    {
        return is_stuttering_gpu;
    }

    TimeBlock* Profiler::GetNewTimeBlock()
    {
        // Increase capacity if needed
        if (m_time_block_index + 1 >= static_cast<int>(m_time_blocks_write.size()))
        {
            increase_capacity = true;
            return nullptr;
        }

        // Return a time block
        return &m_time_blocks_write[++m_time_block_index];
    }

    TimeBlock* Profiler::GetLastIncompleteTimeBlock(TimeBlockType type /*= TimeBlock_Undefined*/)
    {
        for (int i = m_time_block_index; i >= 0; i--)
        {
            TimeBlock& time_block = m_time_blocks_write[i];

            if (type == time_block.GetType() || type == TimeBlockType::Undefined)
            {
                if (!time_block.IsComplete())
                    return &time_block;
            }
        }

        return nullptr;
    }

    void Profiler::AcquireGpuData()
    {
        if (const PhysicalDevice* physical_device = RHI_Device::GetPrimaryPhysicalDevice())
        {
            gpu_name             = physical_device->GetName();
            gpu_memory_used      = RHI_Device::MemoryGetUsageMb();
            gpu_memory_available = RHI_Device::MemoryGetBudgetMb();
            gpu_driver           = physical_device->GetDriverVersion();
            gpu_api              = RHI_Context::api_version_str;
        }
    }

    static string format_float(float value)
    {
        std::stringstream ss;

        // Clamp to a certain range to avoid padding and alignment headaches
        value = Math::Helper::Clamp(value, 0.0f, 99.99f);

        // Set fixed-point notation with 2 decimal places
        ss << std::fixed << std::setprecision(2);

        // Output the integer part with the fill character '0' and the minimum width of 2 characters
        int integer_part = static_cast<int>(value);
        ss << std::setfill('0') << std::setw(2) << integer_part;

        // Output the decimal point and decimal part
        float decimal_part = value - integer_part;
        ss << "." << std::setfill('0') << std::setw(2) << static_cast<int>(round(decimal_part * 100));

        return ss.str();
    }

    void Profiler::DrawPerformanceMetrics()
    {
        metrics_time_since_last_update += static_cast<float>(Timer::GetDeltaTimeSec());
        if (metrics_time_since_last_update < profiling_interval_sec)
        {
            Renderer::DrawString(metrics_str, Math::Vector2(0.01f, 0.01f));
            return;
        }
        metrics_time_since_last_update = 0.0f;
        
        const uint32_t texture_count  = ResourceCache::GetResourceCount(ResourceType::Texture) + ResourceCache::GetResourceCount(ResourceType::Texture2d) + ResourceCache::GetResourceCount(ResourceType::TextureCube);
        const uint32_t material_count = ResourceCache::GetResourceCount(ResourceType::Material);
        const uint32_t pipeline_count = RHI_Device::GetPipelineCount();

        // get the graphics driver vendor
        string api_vendor_name = "AMD";
        if (RHI_Device::GetPrimaryPhysicalDevice()->IsNvidia())
        {
            api_vendor_name = "NVIDIA";
        }

        // clear
        oss_metrics.str("");
        oss_metrics.clear();

        // set fixed-point notation with 2 decimal places
        oss_metrics << std::fixed << std::setprecision(2);

        // overview
        oss_metrics
            << "FPS:\t\t\t" << m_fps << endl
            << "Time:\t\t"  << m_time_frame_avg << " ms" << endl
            << "Frame:\t"   << Renderer::GetFrameNum() << endl;

        // detailed times
        oss_metrics
            << endl     << "\t" << "\t\tavg"                      << "\t" << "\tmin"                        << "\t" << "\tmax"                        << "\t" << "\tlast"                                 << endl
            << "Total:" << "\t" << format_float(m_time_frame_avg) << "\t" << format_float(m_time_frame_min) << "\t" << format_float(m_time_frame_max) << "\t" << format_float(m_time_frame_last) << " ms" << endl
            << "CPU:"   << "\t" << format_float(m_time_cpu_avg)   << "\t" << format_float(m_time_cpu_min)   << "\t" << format_float(m_time_cpu_max)   << "\t" << format_float(m_time_cpu_last)   << " ms" << endl
            << "GPU:"   << "\t" << format_float(m_time_gpu_avg)   << "\t" << format_float(m_time_gpu_min)   << "\t" << format_float(m_time_gpu_max)   << "\t" << format_float(m_time_gpu_last)   << " ms" << endl;

        // gpu
        oss_metrics << endl << "GPU" << endl
            << "Name:\t\t\t"   << gpu_name << endl
            << "Memory:\t"     << gpu_memory_used << "/" << gpu_memory_available << " MB" << endl
            << "API:\t\t\t\t"  << RHI_Context::api_type_str << "\t\t" << gpu_api << endl
            << "Driver:\t\t"   << RHI_Device::GetPrimaryPhysicalDevice()->GetVendorName() << "\t\t" << gpu_driver << endl;

        // display
        oss_metrics << "\nDisplay\n"
            << "Render:\t\t" << static_cast<uint32_t>(Renderer::GetResolutionRender().x) << "x" << static_cast<int>(Renderer::GetResolutionRender().y) << endl
            << "Output:\t\t" << static_cast<uint32_t>(Renderer::GetResolutionOutput().x) << "x" << static_cast<int>(Renderer::GetResolutionOutput().y) << endl
            << "Viewport:\t" << static_cast<uint32_t>(Renderer::GetViewport().width)     << "x" << static_cast<int>(Renderer::GetViewport().height)    << endl
            << "HDR:\t\t\t"  << (Renderer::GetSwapChain()->IsHdr() ? "Enabled" : "Disabled") << endl;

        // cpu
        oss_metrics << endl << "CPU" << endl
            << "Worker threads: " << ThreadPool::GetWorkingThreadCount() << "/" << ThreadPool::GetThreadCount() << endl;

        // api calls
        oss_metrics << "\nAPI calls" << endl;
        if (Profiler::m_granularity == ProfilerGranularity::Full)
        {
            oss_metrics  << "Draw:\t\t\t\t\t\t\t\t\t" << m_rhi_draw << endl;
        }
        oss_metrics
            << "Dispatch:\t\t\t\t\t\t\t"     << m_rhi_dispatch                << endl
            << "Index buffer bindings:\t\t"  << m_rhi_bindings_buffer_index   << endl
            << "Vertex buffer bindings:\t\t" << m_rhi_bindings_buffer_vertex  << endl
            << "Descriptor set bindings:\t"  << m_rhi_bindings_descriptor_set << endl
            << "Pipeline bindings:\t\t\t\t"  << m_rhi_bindings_pipeline       << endl
            << "Pipeline barriers:\t\t\t\t"  << m_rhi_pipeline_barriers       << endl;

        // resources
        oss_metrics << "\nResources\n"
            << "Meshes rendered:\t\t\t\t"   << m_renderer_meshes_rendered << endl
            << "Textures:\t\t\t\t\t\t\t"    << texture_count              << endl
            << "Materials:\t\t\t\t\t\t\t"   << material_count             << endl
            << "Pipelines:\t\t\t\t\t\t\t"   << pipeline_count             << endl
            << "Descriptor set capacity:\t" << m_descriptor_set_count << "/" << m_descriptor_set_capacity;

        // draw at the top-left of the screen
        metrics_str = oss_metrics.str();
        Renderer::DrawString(metrics_str, Math::Vector2(0.01f, 0.01f));
    }
}
