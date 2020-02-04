microcode = '''

def macroop VPBROADCASTQ_ZMM_R {
    mov2fp ufp1, regm, destSize=8, srcSize=8
    vbroadcast64 dest=xmm0, src=ufp1, destVL=64
};

def macroop VPBROADCASTQ_ZMM_XMM {
    vbroadcast64 dest=xmm0, src=xmm0m, destVL=64
};

def macroop VPBROADCASTQ_ZMM_M {
    ldfp ufp1, seg, sib, disp, dataSize=8
    vbroadcast64 dest=xmm0, src=ufp1, destVL=64
};

def macroop VPBROADCASTQ_ZMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, disp, dataSize=8
    vbroadcast64 dest=xmm0, src=ufp1, destVL=64
};

def macroop VPBROADCASTD_ZMM_XMM {
    vbroadcast32 dest=xmm0, src=xmm0m, destVL=64
};

def macroop VPBROADCASTD_ZMM_M {
    ldfp ufp1, seg, sib, disp, dataSize=4
    vbroadcast32 dest=xmm0, src=ufp1, destVL=64
};

def macroop VPBROADCASTD_ZMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, disp, dataSize=4
    vbroadcast32 dest=xmm0, src=ufp1, destVL=64
};

'''