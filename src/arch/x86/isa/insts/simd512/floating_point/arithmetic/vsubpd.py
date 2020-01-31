
microcode = '''
def macroop VSUBPD_XMM_XMM {
    msubf dest=xmm0, src1=xmm0v, op2=xmm0m, size=8, ext=0
    msubf dest=xmm1, src1=xmm1v, op2=xmm1m, size=8, ext=0
};

def macroop VSUBPD_XMM_M {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    msubf dest=xmm0, src1=xmm0v, op2=ufp1, size=8, ext=0
    msubf dest=xmm1, src1=xmm1v, op2=ufp2, size=8, ext=0
};

def macroop VSUBPD_XMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    msubf dest=xmm0, src1=xmm0v, op2=ufp1, size=8, ext=0
    msubf dest=xmm1, src1=xmm1v, op2=ufp2, size=8, ext=0
};

def macroop VSUBPD_YMM_YMM {
    msubf dest=xmm0, src1=xmm0v, op2=xmm0m, size=8, ext=0
    msubf dest=xmm1, src1=xmm1v, op2=xmm1m, size=8, ext=0
    msubf dest=xmm2, src1=xmm2v, op2=xmm2m, size=8, ext=0
    msubf dest=xmm3, src1=xmm3v, op2=xmm3m, size=8, ext=0
};

def macroop VSUBPD_YMM_M {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, sib, "DISPLACEMENT + 24", dataSize=8
    msubf dest=xmm0, src1=xmm0v, op2=ufp1, size=8, ext=0
    msubf dest=xmm1, src1=xmm1v, op2=ufp2, size=8, ext=0
    msubf dest=xmm2, src1=xmm2v, op2=ufp3, size=8, ext=0
    msubf dest=xmm3, src1=xmm3v, op2=ufp4, size=8, ext=0
};

def macroop VSUBPD_YMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    msubf dest=xmm0, src1=xmm0v, op2=ufp1, size=8, ext=0
    msubf dest=xmm1, src1=xmm1v, op2=ufp2, size=8, ext=0
    msubf dest=xmm2, src1=xmm2v, op2=ufp3, size=8, ext=0
    msubf dest=xmm3, src1=xmm3v, op2=ufp4, size=8, ext=0
};

def macroop VSUBPD_ZMM_ZMM {
    msubf dest=xmm0, src1=xmm0v, op2=xmm0m, size=8, ext=0
    msubf dest=xmm1, src1=xmm1v, op2=xmm1m, size=8, ext=0
    msubf dest=xmm2, src1=xmm2v, op2=xmm2m, size=8, ext=0
    msubf dest=xmm3, src1=xmm3v, op2=xmm3m, size=8, ext=0
    msubf dest=xmm4, src1=xmm4v, op2=xmm4m, size=8, ext=0
    msubf dest=xmm5, src1=xmm5v, op2=xmm5m, size=8, ext=0
    msubf dest=xmm6, src1=xmm6v, op2=xmm6m, size=8, ext=0
    msubf dest=xmm7, src1=xmm7v, op2=xmm7m, size=8, ext=0
};

def macroop VSUBPD_ZMM_M {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, sib, "DISPLACEMENT + 24", dataSize=8
    ldfp ufp5, seg, sib, "DISPLACEMENT + 32", dataSize=8
    ldfp ufp6, seg, sib, "DISPLACEMENT + 40", dataSize=8
    ldfp ufp7, seg, sib, "DISPLACEMENT + 48", dataSize=8
    ldfp ufp8, seg, sib, "DISPLACEMENT + 56", dataSize=8
    msubf dest=xmm0, src1=xmm0v, op2=ufp1, size=8, ext=0
    msubf dest=xmm1, src1=xmm1v, op2=ufp2, size=8, ext=0
    msubf dest=xmm2, src1=xmm2v, op2=ufp3, size=8, ext=0
    msubf dest=xmm3, src1=xmm3v, op2=ufp4, size=8, ext=0
    msubf dest=xmm4, src1=xmm4v, op2=ufp5, size=8, ext=0
    msubf dest=xmm5, src1=xmm5v, op2=ufp6, size=8, ext=0
    msubf dest=xmm6, src1=xmm6v, op2=ufp7, size=8, ext=0
    msubf dest=xmm7, src1=xmm7v, op2=ufp8, size=8, ext=0
};

def macroop VSUBPD_ZMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    ldfp ufp5, seg, riprel, "DISPLACEMENT + 32", dataSize=8
    ldfp ufp6, seg, riprel, "DISPLACEMENT + 40", dataSize=8
    ldfp ufp7, seg, riprel, "DISPLACEMENT + 48", dataSize=8
    ldfp ufp8, seg, riprel, "DISPLACEMENT + 56", dataSize=8
    msubf dest=xmm0, src1=xmm0v, op2=ufp1, size=8, ext=0
    msubf dest=xmm1, src1=xmm1v, op2=ufp2, size=8, ext=0
    msubf dest=xmm2, src1=xmm2v, op2=ufp3, size=8, ext=0
    msubf dest=xmm3, src1=xmm3v, op2=ufp4, size=8, ext=0
    msubf dest=xmm4, src1=xmm4v, op2=ufp5, size=8, ext=0
    msubf dest=xmm5, src1=xmm5v, op2=ufp6, size=8, ext=0
    msubf dest=xmm6, src1=xmm6v, op2=ufp7, size=8, ext=0
    msubf dest=xmm7, src1=xmm7v, op2=ufp8, size=8, ext=0
};

'''
