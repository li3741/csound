/*
  pvsops.c: pvs and other spectral-based opcodes

  Copyright (C) 2017 Victor Lazzarini
  This file is part of Csound.

  The Csound Library is free software; you can redistribute it
  and/or modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  Csound is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with Csound; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
  02111-1307 USA
*/
#include <algorithm>
#include <plugin.h>

struct PVTrace : csnd::FPlugin<1, 2> {
  csnd::AuxMem<float> amps;
  static constexpr char const *otypes = "f";
  static constexpr char const *itypes = "fk";

  int init() {
    if (inargs.fsig_data(0).isSliding())
      return csound->init_error(Str("sliding not supported"));

    if (inargs.fsig_data(0).fsig_format() != csnd::fsig_format::pvs &&
        inargs.fsig_data(0).fsig_format() != csnd::fsig_format::polar)
      return csound->init_error(Str("fsig format not supported"));

    amps.allocate(csound, inargs.fsig_data(0).nbins());
    csnd::Fsig &fout = outargs.fsig_data(0);
    fout.init(csound, inargs.fsig_data(0));
    framecount = 0;
    return OK;
  }

  int kperf() {
    csnd::pv_frame &fin = inargs.fsig_data(0);
    csnd::pv_frame &fout = outargs.fsig_data(0);

    if (framecount < fin.count()) {
      int n = fin.len() - (int)inargs[1];
      float thrsh;
      std::transform(fin.begin(), fin.end(), amps.begin(),
                     [](csnd::pv_bin f) { return f.amp(); });
      std::nth_element(amps.begin(), amps.begin() + n, amps.end());
      thrsh = amps[n];
      std::transform(fin.begin(), fin.end(), fout.begin(),
                     [thrsh](csnd::pv_bin f) {
                       return f.amp() >= thrsh ? f : csnd::pv_bin();
                     });
      framecount = fout.count(fin.count());
    }
    return OK;
  }
};

struct TVConv : csnd::Plugin<1, 6> {
  csnd::AuxMem<MYFLT> insp;
  csnd::AuxMem<MYFLT> irsp;
  csnd::AuxMem<MYFLT> out;
  csnd::AuxMem<MYFLT> saved;
  csnd::AuxMem<MYFLT>::iterator itn;
  csnd::AuxMem<MYFLT>::iterator itr;
  uint32_t n;
  uint32_t fils;
  uint32_t pars;
  uint32_t ffts;

  csnd::fftp fwd, inv;
  typedef std::complex<MYFLT> cmplx;

  uint32_t rpow2(uint32_t n) {
    uint32_t v = 2;
    while (v <= n)
      v <<= 1;
    if ((n - (v >> 1)) < (v - n))
      return v >> 1;
    else
      return v;
  }

  cmplx *to_cmplx(MYFLT *f) { return reinterpret_cast<cmplx *>(f); }

  cmplx real_prod(cmplx &a, cmplx &b) {
    return cmplx(a.real() * b.real(), a.imag() * b.imag());
  }

  int init() {
    pars = inargs[4];
    fils = inargs[5];
    if (pars > fils)
      std::swap(pars, fils);
    if (pars > 1) {
      pars = rpow2(pars);
      fils = rpow2(fils) * 2;
      ffts = pars * 2;
      fwd = csound->fft_setup(ffts, FFT_FWD);
      inv = csound->fft_setup(ffts, FFT_INV);
      out.allocate(csound, ffts);
      insp.allocate(csound, fils);
      irsp.allocate(csound, fils);
      saved.allocate(csound, pars);
      n = 0;
    } else {
      insp.allocate(csound, fils);
      irsp.allocate(csound, fils);
    }
    itn = insp.begin();
    itr = irsp.begin();
    return OK;
  }

  int pconv() {
    csnd::AudioSig insig(this, inargs(0));
    csnd::AudioSig irsig(this, inargs(1));
    csnd::AudioSig outsig(this, outargs(0));
    auto irp = irsig.begin();
    auto inp = insig.begin();
    auto frz1 = inargs(2);
    auto frz2 = inargs(3);
    auto inc1 = csound->is_asig(frz1);
    auto inc2 = csound->is_asig(frz2);

    for (auto &s : outsig) {
      if(*frz1 > 0) itn[n] = *inp;
      if(*frz2 > 0) itr[n] = *irp;

      s = out[n] + saved[n];
      saved[n] = out[n + pars];
      if (++n == pars) {
        cmplx *ins, *irs, *ous = to_cmplx(out.data());
	std::fill(itn + pars, itn + ffts, 0.);
	std::fill(itr + pars, itr + ffts, 0.);
	std::fill(out.begin(), out.end(), 0.);
        // FFT
        csound->rfft(fwd, itn);
        csound->rfft(fwd, itr);
        itn += ffts;
	itr += ffts;
        if (itn == insp.end()) itn = insp.begin();
        if (itr == irsp.end()) itr = irsp.begin();
        // spectral delay line
	for (csnd::AuxMem<MYFLT>::iterator in = itn,
	     ir = irsp.end() - ffts; ir >= irsp.begin();
	     in += ffts, ir -= ffts) {
	  if (in == insp.end()) in = insp.begin();
	  ins =  to_cmplx(in);
          irs =  to_cmplx(ir);
          // spectral product
          for (uint32_t i = 1; i < pars; i++)
            ous[i] += ins[i] * irs[i];
          ous[0] += real_prod(ins[0], irs[0]);
        }
        // IFFT
        csound->rfft(inv, out.data());
        n = 0;
      }
      frz1 += inc1;
      frz2 += inc2;
      irp++;
      inp++;
    }
    return OK;
  }

  int dconv() {
    csnd::AudioSig insig(this, inargs(0));
    csnd::AudioSig irsig(this, inargs(1));
    csnd::AudioSig outsig(this, outargs(0));
    auto irp = irsig.begin();
    auto inp = insig.begin();
    auto frz1 = inargs(2);
    auto frz2 = inargs(3);
    auto inc1 = csound->is_asig(frz1);
    auto inc2 = csound->is_asig(frz2);

     for (auto &s : outsig) {
      if(*frz1 > 0) *itn++ = *inp;
      if(*frz2 > 0) *itr++ = *irp;
      if(itn == insp.end()) itn = insp.begin();
      if(itr == irsp.end()) itr = irsp.begin();
      s = 0.;
      for (csnd::AuxMem<MYFLT>::iterator in = itn,
	   ir = irsp.end() - 1; ir >= irsp.begin();
	   in++, ir--) {
	if(in == insp.end()) in  = insp.begin();
        s += *in * *ir;
      }
      frz1 += inc1;
      frz2 += inc2;
      inp++;
      irp++;
    }
    return OK;
  }

  int aperf() {
    if (pars > 1)
      return pconv();
    else
      return dconv();
  }
};

/*
class PrintThread : public csnd::Thread {
  std::atomic_bool splock;
  std::atomic_bool on;
  std::string message;

  void lock() {
    bool tmp = false;
    while(!splock.compare_exchange_weak(tmp,true))
      tmp = false;
  }
  
  void unlock() {
    splock = false;
  }

  uintptr_t run() {
    std::string old;
    while(on) {
      lock();
      if(old.compare(message)) {
       csound->message(message.c_str());
       old = message;
      } 
      unlock(); 
    }
    return 0;
  }
  
public:
  PrintThread(csnd::Csound *csound)
    : Thread(csound), splock(false), on(true), message("") {};

  ~PrintThread(){
    on = false;
    join();
  }
   
  void set_message(const char *m) {
    lock();
    message = m;
    unlock(); 
  }
  
};


struct TPrint : csnd::Plugin<0, 1> {
  static constexpr char const *otypes = "";
  static constexpr char const *itypes = "S";
  PrintThread t;

  int init() {
    csound->plugin_deinit(this);
    csnd::constr(&t, csound);
    return OK;
  }

  int deinit() {
    csnd::destr(&t);
    return OK;
  }

  int kperf() {
    t.set_message(inargs.str_data(0).data);
    return OK;
  }
};
*/



#include <modload.h>
void csnd::on_load(Csound *csound) {
  csnd::plugin<PVTrace>(csound, "pvstrace", csnd::thread::ik);
  csnd::plugin<TVConv>(csound, "tvconv", "a", "aakkii", csnd::thread::ia);
  csnd::plugin<TVConv>(csound, "tvconv", "a", "aaakii", csnd::thread::ia);
  csnd::plugin<TVConv>(csound, "tvconv", "a", "aakaii", csnd::thread::ia);
  csnd::plugin<TVConv>(csound, "tvconv", "a", "aaaaii", csnd::thread::ia);
}
