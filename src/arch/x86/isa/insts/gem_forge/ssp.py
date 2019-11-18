
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
    ssp_stream_flw xmml, imm, dataSize=4, isFloat=True
};

'''