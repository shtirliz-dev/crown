//
// Copyright (C) 2018 Google, Inc.
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//    Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//
//    Redistributions in binary form must reproduce the above
//    copyright notice, this list of conditions and the following
//    disclaimer in the documentation and/or other materials provided
//    with the distribution.
//
//    Neither the name of 3Dlabs Inc. Ltd. nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

//
// Post-processing for SPIR-V IR, in internal form, not standard binary form.
//

#include <cassert>
#include <cstdlib>

#include <unordered_set>
#include <algorithm>

#include "SpvBuilder.h"

#include "spirv.hpp"
#include "GlslangToSpv.h"
#include "SpvBuilder.h"
namespace spv {
    #include "GLSL.std.450.h"
    #include "GLSL.ext.KHR.h"
    #include "GLSL.ext.EXT.h"
#ifdef AMD_EXTENSIONS
    #include "GLSL.ext.AMD.h"
#endif
#ifdef NV_EXTENSIONS
    #include "GLSL.ext.NV.h"
#endif
}

namespace spv {

// Hook to visit each operand type and result type of an instruction.
// Will be called multiple times for one instruction, once for each typed
// operand and the result.
void Builder::postProcessType(const Instruction& inst, Id typeId)
{
    // Characterize the type being questioned
    Id basicTypeOp = getMostBasicTypeClass(typeId);
    int width = 0;
    if (basicTypeOp == OpTypeFloat || basicTypeOp == OpTypeInt)
        width = getScalarTypeWidth(typeId);

    // Do opcode-specific checks
    switch (inst.getOpCode()) {
    case OpLoad:
    case OpStore:
        if (basicTypeOp == OpTypeStruct) {
            if (containsType(typeId, OpTypeInt, 8))
                addCapability(CapabilityInt8);
            if (containsType(typeId, OpTypeInt, 16))
                addCapability(CapabilityInt16);
            if (containsType(typeId, OpTypeFloat, 16))
                addCapability(CapabilityFloat16);
        } else {
            StorageClass storageClass = getStorageClass(inst.getIdOperand(0));
            if (width == 8) {
                switch (storageClass) {
                case StorageClassPhysicalStorageBufferEXT:
                case StorageClassUniform:
                case StorageClassStorageBuffer:
                case StorageClassPushConstant:
                    break;
                default:
                    addCapability(CapabilityInt8);
                    break;
                }
            } else if (width == 16) {
                switch (storageClass) {
                case StorageClassPhysicalStorageBufferEXT:
                case StorageClassUniform:
                case StorageClassStorageBuffer:
                case StorageClassPushConstant:
                case StorageClassInput:
                case StorageClassOutput:
                    break;
                default:
                    if (basicTypeOp == OpTypeInt)
                        addCapability(CapabilityInt16);
                    if (basicTypeOp == OpTypeFloat)
                        addCapability(CapabilityFloat16);
                    break;
                }
            }
        }
        break;
    case OpAccessChain:
    case OpPtrAccessChain:
    case OpCopyObject:
    case OpFConvert:
    case OpSConvert:
    case OpUConvert:
        break;
    case OpExtInst:
#if AMD_EXTENSIONS
        switch (inst.getImmediateOperand(1)) {
        case GLSLstd450Frexp:
        case GLSLstd450FrexpStruct:
            if (getSpvVersion() < glslang::EShTargetSpv_1_3 && containsType(typeId, OpTypeInt, 16))
                addExtension(spv::E_SPV_AMD_gpu_shader_int16);
            break;
        case GLSLstd450InterpolateAtCentroid:
        case GLSLstd450InterpolateAtSample:
        case GLSLstd450InterpolateAtOffset:
            if (getSpvVersion() < glslang::EShTargetSpv_1_3 && containsType(typeId, OpTypeFloat, 16))
                addExtension(spv::E_SPV_AMD_gpu_shader_half_float);
            break;
        default:
            break;
        }
#endif
        break;
    default:
        if (basicTypeOp == OpTypeFloat && width == 16)
            addCapability(CapabilityFloat16);
        if (basicTypeOp == OpTypeInt && width == 16)
            addCapability(CapabilityInt16);
        if (basicTypeOp == OpTypeInt && width == 8)
            addCapability(CapabilityInt8);
        break;
    }
}

// Called for each instruction that resides in a block.
void Builder::postProcess(Instruction& inst)
{
    // Add capabilities based simply on the opcode.
    switch (inst.getOpCode()) {
    case OpExtInst:
        switch (inst.getImmediateOperand(1)) {
        case GLSLstd450InterpolateAtCentroid:
        case GLSLstd450InterpolateAtSample:
        case GLSLstd450InterpolateAtOffset:
            addCapability(CapabilityInterpolationFunction);
            break;
        default:
            break;
        }
        break;
    case OpDPdxFine:
    case OpDPdyFine:
    case OpFwidthFine:
    case OpDPdxCoarse:
    case OpDPdyCoarse:
    case OpFwidthCoarse:
        addCapability(CapabilityDerivativeControl);
        break;

    case OpImageQueryLod:
    case OpImageQuerySize:
    case OpImageQuerySizeLod:
    case OpImageQuerySamples:
    case OpImageQueryLevels:
        addCapability(CapabilityImageQuery);
        break;

#ifdef NV_EXTENSIONS
    case OpGroupNonUniformPartitionNV:
        addExtension(E_SPV_NV_shader_subgroup_partitioned);
        addCapability(CapabilityGroupNonUniformPartitionedNV);
        break;
#endif

    case OpLoad:
    case OpStore:
        {
            // For any load/store to a PhysicalStorageBufferEXT, walk the accesschain
            // index list to compute the misalignment. The pre-existing alignment value
            // (set via Builder::AccessChain::alignment) only accounts for the base of
            // the reference type and any scalar component selection in the accesschain,
            // and this function computes the rest from the SPIR-V Offset decorations.
            Instruction *accessChain = module.getInstruction(inst.getIdOperand(0));
            if (accessChain->getOpCode() == OpAccessChain) {
                Instruction *base = module.getInstruction(accessChain->getIdOperand(0));
                // Get the type of the base of the access chain. It must be a pointer type.
                Id typeId = base->getTypeId();
                Instruction *type = module.getInstruction(typeId);
                assert(type->getOpCode() == OpTypePointer);
                if (type->getImmediateOperand(0) != StorageClassPhysicalStorageBufferEXT) {
                    break;
                }
                // Get the pointee type.
                typeId = type->getIdOperand(1);
                type = module.getInstruction(typeId);
                // Walk the index list for the access chain. For each index, find any
                // misalignment that can apply when accessing the member/element via
                // Offset/ArrayStride/MatrixStride decorations, and bitwise OR them all
                // together.
                int alignment = 0;
                for (int i = 1; i < accessChain->getNumOperands(); ++i) {
                    Instruction *idx = module.getInstruction(accessChain->getIdOperand(i));
                    if (type->getOpCode() == OpTypeStruct) {
                        assert(idx->getOpCode() == OpConstant);
                        unsigned int c = idx->getImmediateOperand(0);

                        const auto function = [&](const std::unique_ptr<Instruction>& decoration) {
                            if (decoration.get()->getOpCode() == OpMemberDecorate &&
                                decoration.get()->getIdOperand(0) == typeId &&
                                decoration.get()->getImmediateOperand(1) == c &&
                                (decoration.get()->getImmediateOperand(2) == DecorationOffset ||
                                 decoration.get()->getImmediateOperand(2) == DecorationMatrixStride)) {
                                alignment |= decoration.get()->getImmediateOperand(3);
                            }
                        };
                        std::for_each(decorations.begin(), decorations.end(), function);
                        // get the next member type
                        typeId = type->getIdOperand(c);
                        type = module.getInstruction(typeId);
                    } else if (type->getOpCode() == OpTypeArray ||
                               type->getOpCode() == OpTypeRuntimeArray) {
                        const auto function = [&](const std::unique_ptr<Instruction>& decoration) {
                            if (decoration.get()->getOpCode() == OpDecorate &&
                                decoration.get()->getIdOperand(0) == typeId &&
                                decoration.get()->getImmediateOperand(1) == DecorationArrayStride) {
                                alignment |= decoration.get()->getImmediateOperand(2);
                            }
                        };
                        std::for_each(decorations.begin(), decorations.end(), function);
                        // Get the element type
                        typeId = type->getIdOperand(0);
                        type = module.getInstruction(typeId);
                    } else {
                        // Once we get to any non-aggregate type, we're done.
                        break;
                    }
                }
                assert(inst.getNumOperands() >= 3);
                unsigned int memoryAccess = inst.getImmediateOperand((inst.getOpCode() == OpStore) ? 2 : 1);
                assert(memoryAccess & MemoryAccessAlignedMask);
                static_cast<void>(memoryAccess);
                // Compute the index of the alignment operand.
                int alignmentIdx = 2;
                if (inst.getOpCode() == OpStore)
                    alignmentIdx++;
                // Merge new and old (mis)alignment
                alignment |= inst.getImmediateOperand(alignmentIdx);
                // Pick the LSB
                alignment = alignment & ~(alignment & (alignment-1));
                // update the Aligned operand
                inst.setImmediateOperand(alignmentIdx, alignment);
            }
            break;
        }

    default:
        break;
    }

    // Checks based on type
    if (inst.getTypeId() != NoType)
        postProcessType(inst, inst.getTypeId());
    for (int op = 0; op < inst.getNumOperands(); ++op) {
        if (inst.isIdOperand(op)) {
            // In blocks, these are always result ids, but we are relying on
            // getTypeId() to return NoType for things like OpLabel.
            if (getTypeId(inst.getIdOperand(op)) != NoType)
                postProcessType(inst, getTypeId(inst.getIdOperand(op)));
        }
    }
}

// Called for each instruction in a reachable block.
void Builder::postProcessReachable(const Instruction&)
{
    // did have code here, but questionable to do so without deleting the instructions
}

// comment in header
void Builder::postProcess()
{
    std::unordered_set<const Block*> reachableBlocks;
    std::unordered_set<Id> unreachableDefinitions;
    // Collect IDs defined in unreachable blocks. For each function, label the
    // reachable blocks first. Then for each unreachable block, collect the
    // result IDs of the instructions in it.
    for (auto fi = module.getFunctions().cbegin(); fi != module.getFunctions().cend(); fi++) {
        Function* f = *fi;
        Block* entry = f->getEntryBlock();
        inReadableOrder(entry, [&reachableBlocks](const Block* b) { reachableBlocks.insert(b); });
        for (auto bi = f->getBlocks().cbegin(); bi != f->getBlocks().cend(); bi++) {
            Block* b = *bi;
            if (reachableBlocks.count(b) == 0) {
                for (auto ii = b->getInstructions().cbegin(); ii != b->getInstructions().cend(); ii++)
                    unreachableDefinitions.insert(ii->get()->getResultId());
            }
        }
    }

    // Remove unneeded decorations, for unreachable instructions
    decorations.erase(std::remove_if(decorations.begin(), decorations.end(),
        [&unreachableDefinitions](std::unique_ptr<Instruction>& I) -> bool {
            Id decoration_id = I.get()->getIdOperand(0);
            return unreachableDefinitions.count(decoration_id) != 0;
        }),
        decorations.end());

    // Add per-instruction capabilities, extensions, etc.,

    // process all reachable instructions...
    for (auto bi = reachableBlocks.cbegin(); bi != reachableBlocks.cend(); ++bi) {
        const Block* block = *bi;
        const auto function = [this](const std::unique_ptr<Instruction>& inst) { postProcessReachable(*inst.get()); };
        std::for_each(block->getInstructions().begin(), block->getInstructions().end(), function);
    }

    // process all block-contained instructions
    for (auto fi = module.getFunctions().cbegin(); fi != module.getFunctions().cend(); fi++) {
        Function* f = *fi;
        for (auto bi = f->getBlocks().cbegin(); bi != f->getBlocks().cend(); bi++) {
            Block* b = *bi;
            for (auto ii = b->getInstructions().cbegin(); ii != b->getInstructions().cend(); ii++)
                postProcess(*ii->get());

            // For all local variables that contain pointers to PhysicalStorageBufferEXT, check whether
            // there is an existing restrict/aliased decoration. If we don't find one, add Aliased as the
            // default.
            for (auto vi = b->getLocalVariables().cbegin(); vi != b->getLocalVariables().cend(); vi++) {
                const Instruction& inst = *vi->get();
                Id resultId = inst.getResultId();
                if (containsPhysicalStorageBufferOrArray(getDerefTypeId(resultId))) {
                    bool foundDecoration = false;
                    const auto function = [&](const std::unique_ptr<Instruction>& decoration) {
                        if (decoration.get()->getIdOperand(0) == resultId &&
                            decoration.get()->getOpCode() == OpDecorate &&
                            (decoration.get()->getImmediateOperand(1) == spv::DecorationAliasedPointerEXT ||
                             decoration.get()->getImmediateOperand(1) == spv::DecorationRestrictPointerEXT)) {
                            foundDecoration = true;
                        }
                    };
                    std::for_each(decorations.begin(), decorations.end(), function);
                    if (!foundDecoration) {
                        addDecoration(resultId, spv::DecorationAliasedPointerEXT);
                    }
                }
            }
        }
    }

    // Look for any 8/16 bit type in physical storage buffer class, and set the
    // appropriate capability. This happens in createSpvVariable for other storage
    // classes, but there isn't always a variable for physical storage buffer.
    for (int t = 0; t < (int)groupedTypes[OpTypePointer].size(); ++t) {
        Instruction* type = groupedTypes[OpTypePointer][t];
        if (type->getImmediateOperand(0) == (unsigned)StorageClassPhysicalStorageBufferEXT) {
            if (containsType(type->getIdOperand(1), OpTypeInt, 8)) {
                addExtension(spv::E_SPV_KHR_8bit_storage);
                addCapability(spv::CapabilityStorageBuffer8BitAccess);
            }
            if (containsType(type->getIdOperand(1), OpTypeInt, 16) ||
                containsType(type->getIdOperand(1), OpTypeFloat, 16)) {
                addExtension(spv::E_SPV_KHR_16bit_storage);
                addCapability(spv::CapabilityStorageBuffer16BitAccess);
            }
        }
    }
}

}; // end spv namespace
