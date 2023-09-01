microcode = """

def macroop VBROADCASTSS_XMM_XMM {
    vbroadcast srcSize=4, dest=xmm0, src=xmm0m, destVL=16
    vclear dest=xmm2, destVL=16
};

def macroop VBROADCASTSS_XMM_M {
    ldfp ufp1, seg, sib, disp, dataSize=4
    vbroadcast srcSize=4, dest=xmm0, src=ufp1, destVL=16
    vclear dest=xmm2, destVL=16
};

def macroop VBROADCASTSS_XMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, disp, dataSize=4
    vbroadcast srcSize=4, dest=xmm0, src=ufp1, destVL=16
    vclear dest=xmm2, destVL=16
};

def macroop VBROADCASTSS_YMM_XMM {
    vbroadcast srcSize=4, dest=xmm0, src=xmm0m, destVL=32, zeroHighRegs=1
};

def macroop VBROADCASTSS_YMM_M {
    ldfp32to256zero512 xmm0, seg, sib, disp, dataSize=4
};

def macroop VBROADCASTSS_YMM_P {
    rdip t7
    ldfp32to256zero512 xmm0, seg, riprel, disp, dataSize=4
};

def macroop VBROADCASTSS_ZMM_XMM {
    vbroadcast srcSize=4, dest=xmm0, src=xmm0m, destVL=64
};

def macroop VBROADCASTSS_ZMM_M {
    ldfp32to512 xmm0, seg, sib, disp, dataSize=4
};

def macroop VBROADCASTSS_ZMM_P {
    rdip t7
    ldfp32to512 xmm0, seg, riprel, disp, dataSize=4
};

"""
