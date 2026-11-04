#pragma once

#include <complex>
#include <vector>

namespace glina {

// Small iterative radix-2 FFT (double precision). Good enough for analysis;
// the plugin build can swap in juce::dsp::FFT / pffft later without API change.
class FFT {
public:
    explicit FFT(int n);
    int size() const { return n_; }
    void forward(std::vector<std::complex<double>>& a) const;
    void inverse(std::vector<std::complex<double>>& a) const; // includes 1/n scaling

    static int nextPow2(int v);

private:
    void transform(std::vector<std::complex<double>>& a, bool inv) const;
    int n_;
    std::vector<int> rev_;
    std::vector<std::complex<double>> tw_;
};

} // namespace glina
