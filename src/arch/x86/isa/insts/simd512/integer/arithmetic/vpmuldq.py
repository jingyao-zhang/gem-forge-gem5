microcode = '''
def macroop VPMULDQ_XMM_XMM {
    vmului dest=xmm0, src1=xmm0v, src2=xmm0m, size=8, VL=16
};

def macroop VPMULDQ_XMM_M {
    ldfp128 ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=16
    vmului dest=xmm0, src1=xmm0v, src2=ufp1, size=8, VL=16
};

def macroop VPMULDQ_XMM_P {
    rdip t7
    ldfp128 ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=16
    vmului dest=xmm0, src1=xmm0v, src2=ufp1, size=8, VL=16
};

def macroop VPMULDQ_YMM_YMM {
    vmului dest=xmm0, src1=xmm0v, src2=xmm0m, size=8, VL=32
};

def macroop VPMULDQ_YMM_M {
    ldfp256 ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=32
    vmului dest=xmm0, src1=xmm0v, src2=ufp1, size=8, VL=32
};

def macroop VPMULDQ_YMM_P {
    rdip t7
    ldfp256 ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=32
    vmului dest=xmm0, src1=xmm0v, src2=ufp1, size=8, VL=32
};

def macroop VPMULDQ_ZMM_ZMM {
    vmului dest=xmm0, src1=xmm0v, src2=xmm0m, size=8, VL=64
};

def macroop VPMULDQ_ZMM_M {
    ldfp512 ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=64
    vmului dest=xmm0, src1=xmm0v, src2=ufp1, size=8, VL=64
};

def macroop VPMULDQ_ZMM_P {
    rdip t7
    ldfp512 ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=64
    vmului dest=xmm0, src1=xmm0v, src2=ufp1, size=8, VL=64
};
'''