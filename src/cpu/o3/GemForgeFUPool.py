# Copyright (c) 2017 ARM Limited
# All rights reserved
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2006-2007 The Regents of The University of Michigan
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# Authors: Kevin Lim

from m5.SimObject import SimObject
from m5.params import *
from m5.objects.FuncUnit import *
from m5.objects.FuncUnitConfig import *
from m5.objects.FUPool import *

class GemForgeIntSIMDUnit(FUDesc):
    opList = [ OpDesc(opClass='SimdAdd'),
               OpDesc(opClass='SimdAddAcc'),
               OpDesc(opClass='SimdAlu'),
               OpDesc(opClass='SimdCmp'),
               OpDesc(opClass='SimdCvt'),
               OpDesc(opClass='SimdMisc'),
               OpDesc(opClass='SimdMult'),
               OpDesc(opClass='SimdMultAcc'),
               OpDesc(opClass='SimdShift'),
               OpDesc(opClass='SimdShiftAcc'),
               OpDesc(opClass='SimdDiv'),
               OpDesc(opClass='SimdSqrt')]
    count = 2

class GemForgeFpSIMDUnit(FUDesc):
    opList = [ OpDesc(opLat=2, opClass='SimdFloatAdd'),
               OpDesc(opLat=2, opClass='SimdFloatAlu'),
               OpDesc(opLat=2, opClass='SimdFloatCmp'),
               OpDesc(opLat=2, opClass='SimdFloatCvt'),
               OpDesc(opLat=2, opClass='SimdFloatDiv'),
               OpDesc(opLat=2, opClass='SimdFloatMisc'),
               OpDesc(opLat=2, opClass='SimdFloatMult'),
               OpDesc(opLat=2, opClass='SimdFloatMultAcc'),
               OpDesc(opLat=2, opClass='SimdFloatSqrt'),
               OpDesc(opLat=2, opClass='SimdReduceAdd'),
               OpDesc(opLat=2, opClass='SimdReduceAlu'),
               OpDesc(opLat=2, opClass='SimdReduceCmp'),
               OpDesc(opLat=2, opClass='SimdFloatReduceAdd'),
               OpDesc(opLat=2, opClass='SimdFloatReduceCmp') ]
    count = 2

class GemForgeO4FUPool(FUPool):
    FUList = [ IntALU(), IntMultDiv(), FP_ALU(), FP_MultDiv(), ReadPort(),
               GemForgeIntSIMDUnit(), GemForgeFpSIMDUnit(),
               PredALU(), WritePort(), RdWrPort(), IprPort() ]

class GemForgeO8FUPool(FUPool):
    FUList = [ IntALU(), IntMultDiv(), FP_ALU(), FP_MultDiv(), ReadPort(),
               GemForgeIntSIMDUnit(), GemForgeFpSIMDUnit(),
               PredALU(), WritePort(), RdWrPort(), IprPort() ]
    FUList[0].count = 8
    FUList[1].count = 4
    FUList[2].count = 4
    FUList[3].count = 4
    FUList[5].count = 4
    FUList[6].count = 4
