microcode = '''

def macroop VCVTPD2PS128_XMM_XMM {
    cvtf2f xmm0, xmm0m, destSize=4, srcSize=8, ext=0
    cvtf2f xmm0, xmm1m, destSize=4, srcSize=8, ext=2
    lfpimm xmm1, 0
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VCVTPD2PS128_XMM_M {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    cvtf2f xmm0, ufp1, destSize=4, srcSize=8, ext=0
    cvtf2f xmm0, ufp2, destSize=4, srcSize=8, ext=2
    lfpimm xmm1, 0
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VCVTPD2PS128_XMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    cvtf2f xmm0, ufp1, destSize=4, srcSize=8, ext=0
    cvtf2f xmm0, ufp2, destSize=4, srcSize=8, ext=2
    lfpimm xmm1, 0
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VCVTPD2PS256_XMM_YMM {
    cvtf2f xmm0, xmm0m, destSize=4, srcSize=8, ext=0
    cvtf2f xmm0, xmm1m, destSize=4, srcSize=8, ext=2
    cvtf2f xmm1, xmm2m, destSize=4, srcSize=8, ext=0
    cvtf2f xmm1, xmm3m, destSize=4, srcSize=8, ext=2
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VCVTPD2PS256_XMM_M {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, sib, "DISPLACEMENT + 24", dataSize=8
    cvtf2f xmm0, ufp1, destSize=4, srcSize=8, ext=0
    cvtf2f xmm0, ufp2, destSize=4, srcSize=8, ext=2
    cvtf2f xmm1, ufp3, destSize=4, srcSize=8, ext=0
    cvtf2f xmm1, ufp4, destSize=4, srcSize=8, ext=2
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VCVTPD2PS256_XMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    cvtf2f xmm0, ufp1, destSize=4, srcSize=8, ext=0
    cvtf2f xmm0, ufp2, destSize=4, srcSize=8, ext=2
    cvtf2f xmm1, ufp3, destSize=4, srcSize=8, ext=0
    cvtf2f xmm1, ufp4, destSize=4, srcSize=8, ext=2
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VCVTPD2PS512_YMM_ZMM {
    cvtf2f xmm0, xmm0m, destSize=4, srcSize=8, ext=0
    cvtf2f xmm0, xmm1m, destSize=4, srcSize=8, ext=2
    cvtf2f xmm1, xmm2m, destSize=4, srcSize=8, ext=0
    cvtf2f xmm1, xmm3m, destSize=4, srcSize=8, ext=2
    cvtf2f xmm2, xmm4m, destSize=4, srcSize=8, ext=0
    cvtf2f xmm2, xmm5m, destSize=4, srcSize=8, ext=0
    cvtf2f xmm3, xmm6m, destSize=4, srcSize=8, ext=2
    cvtf2f xmm3, xmm7m, destSize=4, srcSize=8, ext=2
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VCVTPD2PS512_YMM_M {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, sib, "DISPLACEMENT + 24", dataSize=8
    ldfp ufp5, seg, sib, "DISPLACEMENT + 32", dataSize=8
    ldfp ufp6, seg, sib, "DISPLACEMENT + 40", dataSize=8
    ldfp ufp7, seg, sib, "DISPLACEMENT + 48", dataSize=8
    ldfp ufp8, seg, sib, "DISPLACEMENT + 56", dataSize=8
    cvtf2f xmm0, ufp1, destSize=4, srcSize=8, ext=0
    cvtf2f xmm0, ufp2, destSize=4, srcSize=8, ext=2
    cvtf2f xmm1, ufp3, destSize=4, srcSize=8, ext=0
    cvtf2f xmm1, ufp4, destSize=4, srcSize=8, ext=2
    cvtf2f xmm2, ufp5, destSize=4, srcSize=8, ext=0
    cvtf2f xmm2, ufp6, destSize=4, srcSize=8, ext=0
    cvtf2f xmm3, ufp7, destSize=4, srcSize=8, ext=2
    cvtf2f xmm3, ufp8, destSize=4, srcSize=8, ext=2
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VCVTPD2PS512_YMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    ldfp ufp5, seg, riprel, "DISPLACEMENT + 32", dataSize=8
    ldfp ufp6, seg, riprel, "DISPLACEMENT + 40", dataSize=8
    ldfp ufp7, seg, riprel, "DISPLACEMENT + 48", dataSize=8
    ldfp ufp8, seg, riprel, "DISPLACEMENT + 56", dataSize=8
    cvtf2f xmm0, ufp1, destSize=4, srcSize=8, ext=0
    cvtf2f xmm0, ufp2, destSize=4, srcSize=8, ext=2
    cvtf2f xmm1, ufp3, destSize=4, srcSize=8, ext=0
    cvtf2f xmm1, ufp4, destSize=4, srcSize=8, ext=2
    cvtf2f xmm2, ufp5, destSize=4, srcSize=8, ext=0
    cvtf2f xmm2, ufp6, destSize=4, srcSize=8, ext=0
    cvtf2f xmm3, ufp7, destSize=4, srcSize=8, ext=2
    cvtf2f xmm3, ufp8, destSize=4, srcSize=8, ext=2
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

'''