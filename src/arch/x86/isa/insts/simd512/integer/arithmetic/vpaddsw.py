microcode = '''
def macroop VPADDSW_XMM_XMM {
    vaddi dest=xmm0, src1=xmm0v, src2=xmm0m, size=2, VL=16
};

def macroop VPADDSW_XMM_M {
    ldfp128 ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=16
    vaddi dest=xmm0, src1=xmm0v, src2=ufp1, size=2, VL=16
};

def macroop VPADDSW_XMM_P {
    rdip t7
    ldfp128 ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=16
    vaddi dest=xmm0, src1=xmm0v, src2=ufp1, size=2, VL=16
};

def macroop VPADDSW_YMM_YMM {
    vaddi dest=xmm0, src1=xmm0v, src2=xmm0m, size=2, VL=32
};

def macroop VPADDSW_YMM_M {
    ldfp256 ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=32
    vaddi dest=xmm0, src1=xmm0v, src2=ufp1, size=2, VL=32
};

def macroop VPADDSW_YMM_P {
    rdip t7
    ldfp256 ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=32
    vaddi dest=xmm0, src1=xmm0v, src2=ufp1, size=2, VL=32
};

def macroop VPADDSW_ZMM_ZMM {
    vaddi dest=xmm0, src1=xmm0v, src2=xmm0m, size=2, VL=64
};

def macroop VPADDSW_ZMM_M {
    ldfp512 ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=64
    vaddi dest=xmm0, src1=xmm0v, src2=ufp1, size=2, VL=64
};

def macroop VPADDSW_ZMM_P {
    rdip t7
    ldfp512 ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=64
    vaddi dest=xmm0, src1=xmm0v, src2=ufp1, size=2, VL=64
};
'''