#ifndef SCOPE_REDIRECT_H_
#define SCOPE_REDIRECT_H_

#include <fstream>
#include <iostream>

class ScopedRedirect {
public:
  ScopedRedirect(std::ostream &inOriginal, std::ostream &inRedirect)
      : mOriginal(inOriginal),
        mOldBuffer(inOriginal.rdbuf(inRedirect.rdbuf())) {}

  ~ScopedRedirect() { mOriginal.rdbuf(mOldBuffer); }

private:
  ScopedRedirect(const ScopedRedirect &);
  ScopedRedirect &operator=(const ScopedRedirect &);

  std::ostream &mOriginal;
  std::streambuf *mOldBuffer;
};

#endif