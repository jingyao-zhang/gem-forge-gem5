
microcode = '''
def macroop SSP_STREAM_CONFIG_I
{
    ssp_stream_config imm
};

def macroop SSP_STREAM_END_I
{
    ssp_stream_end imm
};

def macroop SSP_STREAM_INPUT_R_I
{
    ssp_stream_input reg, imm
};

def macroop SSP_STREAM_INPUT_ZMM_I
{
    ssp_stream_input xmm0, imm, dataSize=64
};

def macroop SSP_STREAM_INPUT_YMM_I
{
    ssp_stream_input xmm0, imm, dataSize=32
};

def macroop SSP_STREAM_INPUT_XMM_I
{
    ssp_stream_input xmm0, imm, dataSize=16
};

def macroop SSP_STREAM_INPUT_M_I
{
    panic "SSP_STREAM_INPUT with memory operand in R/M."
};

def macroop SSP_STREAM_INPUT_P_I
{
    panic "SSP_STREAM_INPUT with P operand in R/M."
};

def macroop SSP_STREAM_READY
{
    ssp_stream_ready
};

def macroop SSP_STREAM_STEP_I
{
    ssp_stream_step imm
};

def macroop SSP_STREAM_STORE_I
{
    ssp_stream_store imm
};

def macroop SSP_STREAM_LOAD_R_I
{
    ssp_stream_load reg, imm
};

def macroop SSP_STREAM_LOAD_M_I
{
    panic "SSP_STREAM_LOAD with memory operand in R/M."
};

def macroop SSP_STREAM_LOAD_P_I
{
    panic "SSP_STREAM_LOAD with P operand in R/M."
};

def macroop SSP_STREAM_LOAD_XMM_I
{
    ssp_stream_fload xmm0, imm, dataSize="env.dataSize", isFloat=True
};

def macroop SSP_STREAM_LOAD_YMM_I
{
    ssp_stream_fload xmm0, imm, dataSize=32, isFloat=True
};

def macroop SSP_STREAM_LOAD_ZMM_I
{
    ssp_stream_fload xmm0, imm, dataSize=64, isFloat=True
};

def macroop SSP_STREAM_ATOMIC_R_I
{
    ssp_stream_atomic reg, imm, isAtomic=True
};

def macroop SSP_STREAM_ATOMIC_M_I
{
    panic "SSP_STREAM_ATOMIC with memory operand in R/M."
};

def macroop SSP_STREAM_ATOMIC_P_I
{
    panic "SSP_STREAM_ATOMIC with P operand in R/M."
};

def macroop SSP_STREAM_ATOMIC_XMM_I
{
    ssp_stream_atomic xmm0, imm, dataSize="env.dataSize", isFloat=True, isAtomic=True
};

'''