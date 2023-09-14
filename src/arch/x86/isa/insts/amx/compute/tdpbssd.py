
microcode = """

def macroop TDPBSSD_ZMM_ZMM {
    tdpbssd dest=tmm0, src1=tmm0v, src2=tmm0m, size=8, VL=64
};

def macroop TDPBSSD_ZMM_M {
    panic "TDPBSSD with M operand in R/M"
};

def macroop TDPBSSD_ZMM_P {
    panic "TDPBSSD with P operand in R/M"
};

"""
