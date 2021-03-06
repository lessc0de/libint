/*
 *  This file is a part of Libint.
 *  Copyright (C) 2004-2014 Edward F. Valeev
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Library General Public License, version 2,
 *  as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this program.  If not, see http://www.gnu.org/licenses/.
 *
 */

#ifndef _libint2_src_lib_libint_engine_h_
#define _libint2_src_lib_libint_engine_h_

#if __cplusplus <= 199711L
# error "The simple Libint API requires C++11 support"
#endif

#include <iostream>
#include <array>
#include <vector>

#include <libint2.h>
#include <libint2/boys.h>
#include <libint2/shell.h>

#include <Eigen/Core>

// uncomment to troubleshoot solid harmonics transform
//#define FORCE_SOLID_TFORM_CHECK

namespace libint2 {

#ifdef LIBINT2_SUPPORT_ONEBODY
  /// OneBodyEngine computes integrals of 1-body operators, e.g. overlap, kinetic energy, dipole moment, etc.

  /**
   * OneBodyEngine computes integrals of types given by OneBodyEngine::type
   */
  class OneBodyEngine {
    public:
      enum type {
        overlap,
        kinetic,
        nuclear,
        _invalid
      };

      /// creates a default (unusable) OneBodyEngine; to be used as placeholder for copying a usable engine
      OneBodyEngine() : type_(_invalid), primdata_(), lmax_(-1) {}

      /// Constructs a (usable) OneBodyEngine

      /// \param t integral type, see OneBodyEngine::type
      /// \param max_nprim the maximum number of primitives per contracted Gaussian shell
      /// \param max_l the maximum angular momentum of Gaussian shell
      /// \param deriv_level if not 0, will compute geometric derivatives of Gaussian integrals of order \c deriv_level
      /// \note if type == nuclear, must specify charges using set_q()
      /// \warning currently only the following types are suported: \c overlap, \c kinetic, \c nuclear
      /// \warning currently derivative integrals are not supported
      /// \warning currently solid harmonics Gaussians are not supported
      OneBodyEngine(type t, size_t max_nprim, int max_l, int deriv_order = 0) :
        type_(t), primdata_(max_nprim * max_nprim), lmax_(max_l), deriv_order_(deriv_order),
        fm_eval_(t == nuclear ? libint2::FmEval_Chebyshev3::instance(2*max_l+deriv_order) : 0) {
        initialize();
      }

      /// move constructor
      OneBodyEngine(OneBodyEngine&& other) = default;

      /// (deep) copy constructor
      OneBodyEngine(const OneBodyEngine& other) :
        type_(other.type_),
        primdata_(other.primdata_.size()),
        lmax_(other.lmax_),
        deriv_order_(other.deriv_order_),
        q_(other.q_),
        fm_eval_(other.fm_eval_) {
        initialize();
      }

      ~OneBodyEngine() {
        finalize();
      }

      /// move assignment
      OneBodyEngine& operator=(OneBodyEngine&& other) = default;

      /// (deep) copy assignment
      OneBodyEngine& operator=(const OneBodyEngine& other) {
        type_ = other.type_;
        primdata_.resize(other.primdata_.size());
        lmax_ = other.lmax_;
        deriv_order_ = other.deriv_order_;
        q_ = other.q_;
        fm_eval_ = other.fm_eval_;
        initialize();
        return *this;
      }

      /// specifies the nuclear charges
      /// \param q vector of {charge,Cartesian coordinate} pairs
      void set_q(const std::vector<std::pair<double, std::array<double, 3>>>& q) {
        q_ = q;
      }

      /// computes shell set of integrals
      /// \note result is stored in row-major order
      const LIBINT2_REALTYPE* compute(const libint2::Shell& s1,
                                      const libint2::Shell& s2) {

        // can only handle 1 contraction at a time
        assert(s1.ncontr() == 1 && s2.ncontr() == 1);
        // derivatives not supported for now
        assert(deriv_order_ == 0);

        const auto l1 = s1.contr[0].l;
        const auto l2 = s2.contr[0].l;

        // if want nuclear, make sure there is at least one nucleus .. otherwise the user likely forgot to call set_q
        if (type_ == nuclear and q_.size() == 0)
          throw std::runtime_error("libint2::OneBodyEngine(type = nuclear), but no nuclei found; forgot to call set_q()?");

#if LIBINT2_SHELLQUARTET_SET == LIBINT2_SHELLQUARTET_SET_STANDARD // make sure bra.l >= ket.l
        const auto swap = (l1 < l2);
#else // make sure bra.l <= ket.l
        const auto swap = (l1 > l2);
#endif
        const auto& bra = !swap ? s1 : s2;
        const auto& ket = !swap ? s2 : s1;

        const auto n1 = s1.size();
        const auto n2 = s2.size();
        const auto ncart1 = s1.cartesian_size();
        const auto ncart2 = s2.cartesian_size();

        const bool use_scratch = (swap || type_ == nuclear);

        // assert # of primitive pairs
        const auto nprim_bra = bra.nprim();
        const auto nprim_ket = ket.nprim();
        const auto nprimpairs = nprim_bra * nprim_ket;
        assert(nprimpairs <= primdata_.size());

        // adjust max angular momentum, if needed
        const auto lmax = std::max(l1, l2);
        assert (lmax <= lmax_);
        if (lmax == 0) // (s|s) ints will be accumulated in the first element of stack
          primdata_[0].stack[0] = 0;
        else if (use_scratch)
          memset(static_cast<void*>(&scratch_[0]), 0, sizeof(LIBINT2_REALTYPE)*ncart1*ncart2);

        // loop over operator components
        const auto num_operset = type_ == nuclear ? q_.size() : 1u;
        for(auto oset=0u; oset!=num_operset; ++oset) {

            auto p12 = 0;
            for(auto pb=0; pb!=nprim_bra; ++pb) {
              for(auto pk=0; pk!=nprim_ket; ++pk, ++p12) {
                compute_primdata(primdata_[p12],bra,ket,pb,pk,oset);
              }
            }
            primdata_[0].contrdepth = p12;

            if (lmax == 0) { // (s|s)
              auto& result = primdata_[0].stack[0];
              switch (type_) {
                case overlap:
                for(auto p12=0; p12 != primdata_[0].contrdepth; ++p12)
                  result += primdata_[p12].LIBINT_T_S_OVERLAP_S[0];
                  break;
                case kinetic:
                for(auto p12=0; p12 != primdata_[0].contrdepth; ++p12)
                  result += primdata_[p12].LIBINT_T_S_KINETIC_S[0];
                  break;
                case nuclear:
                for(auto p12=0; p12 != primdata_[0].contrdepth; ++p12)
                  result += primdata_[p12].LIBINT_T_S_ELECPOT_S(0)[0];
                  break;
                default:
                  assert(false);
              }
              primdata_[0].targets[0] = primdata_[0].stack;
            }
            else {
              switch (type_) {
                case overlap:
                  LIBINT2_PREFIXED_NAME(libint2_build_overlap)[bra.contr[0].l][ket.contr[0].l](&primdata_[0]);
                  break;
                case kinetic:
                  LIBINT2_PREFIXED_NAME(libint2_build_kinetic)[bra.contr[0].l][ket.contr[0].l](&primdata_[0]);
                  break;
                case nuclear:
                  LIBINT2_PREFIXED_NAME(libint2_build_elecpot)[bra.contr[0].l][ket.contr[0].l](&primdata_[0]);
                  break;
                default:
                  assert(false);
              }
              if (use_scratch) {
                const auto ncart_bra = bra.cartesian_size();
                const auto ncart_ket = ket.cartesian_size();
                constexpr auto using_scalar_real = std::is_same<double,LIBINT2_REALTYPE>::value || std::is_same<float,LIBINT2_REALTYPE>::value;
                static_assert(using_scalar_real, "Libint2 C++11 API only supports fundamental real types");
                typedef Eigen::Matrix<LIBINT2_REALTYPE, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor > Matrix;
                Eigen::Map<Matrix> braket(primdata_[0].targets[0], ncart_bra, ncart_ket);
                Eigen::Map<Matrix> set12(&scratch_[0], ncart1, ncart2);
                if (swap)
                  set12 += braket.transpose();
                else
                  set12 += braket;
              }
            } // ltot != 0

        } // oset (operator components, artifact of nuclear)

        auto cartesian_ints = (use_scratch && lmax != 0) ? &scratch_[0] : primdata_[0].targets[0];

        auto result = cartesian_ints;

        if (s1.contr[0].pure || s2.contr[0].pure) {
          auto* spherical_ints = (cartesian_ints == &scratch_[0]) ? primdata_[0].targets[0] : &scratch_[0];
          if (s1.contr[0].pure && s2.contr[0].pure) {
            libint2::solidharmonics::tform(l1, l2, cartesian_ints, spherical_ints);
          }
          else {
            if (s1.contr[0].pure)
              libint2::solidharmonics::tform_rows(l1, n2, cartesian_ints, spherical_ints);
            else
              libint2::solidharmonics::tform_cols(n1, l2, cartesian_ints, spherical_ints);
          }

#ifdef FORCE_SOLID_TFORM_CHECK
          if (1) {
            const size_t nreplicas = 7;
            const auto blksize = n1*n2;
            const auto cart_blksize = ncart1*ncart2;
            LIBINT2_REALTYPE* test_cartesian_ints = new LIBINT2_REALTYPE[nreplicas*cart_blksize];
            for(auto i12=0, i12r=0; i12<cart_blksize; ++i12) {
              for(auto r=0; r<nreplicas; ++r, ++i12r) {
                test_cartesian_ints[i12r] = cartesian_ints[i12] * r;
              }
            }
            LIBINT2_REALTYPE* test_spherical_ints = new LIBINT2_REALTYPE[nreplicas*blksize];
            libint2::solidharmonics::tform_tensor(s1.contr[0], s2.contr[0], nreplicas, test_cartesian_ints, test_spherical_ints);
            bool tform_tensor_works = true;
            for(auto i12=0, i12r=0; i12<blksize; ++i12) {
              for(auto r=0; r<nreplicas; ++r, ++i12r) {
                if (::fabs(test_spherical_ints[i12r] - spherical_ints[i12] * r) > 1e-12) {
                  tform_tensor_works = false;
                  throw "sanity test of tform_tensor failed!";
                }
              }
            }

          }
#endif

          result = spherical_ints;
        } // tform to solids

        return result;
      }

      void compute_primdata(Libint_t& primdata,
                            const Shell& s1, const Shell& s2,
                            size_t p1, size_t p2,
                            size_t oset) {

        const auto& A = s1.O;
        const auto& B = s2.O;

        const auto alpha1 = s1.alpha[p1];
        const auto alpha2 = s2.alpha[p2];

        const auto c1 = s1.contr[0].coeff[p1];
        const auto c2 = s2.contr[0].coeff[p2];

        const auto gammap = alpha1 + alpha2;
        const auto oogammap = 1.0 / gammap;
        const auto rhop = alpha1 * alpha2 * oogammap;
        const auto Px = (alpha1 * A[0] + alpha2 * B[0]) * oogammap;
        const auto Py = (alpha1 * A[1] + alpha2 * B[1]) * oogammap;
        const auto Pz = (alpha1 * A[2] + alpha2 * B[2]) * oogammap;
        const auto AB_x = A[0] - B[0];
        const auto AB_y = A[1] - B[1];
        const auto AB_z = A[2] - B[2];
        const auto AB2 = AB_x*AB_x + AB_y*AB_y + AB_z*AB_z;

        if (LIBINT2_SHELLQUARTET_SET == LIBINT2_SHELLQUARTET_SET_STANDARD // always VRR on bra, and HRR to bra (overlap, coulomb)
            || type_ == kinetic // kinetic energy ints don't use HRR, hence VRR on both centers
           ) {

#if LIBINT2_DEFINED(eri,PA_x)
        primdata.PA_x[0] = Px - A[0];
#endif
#if LIBINT2_DEFINED(eri,PA_y)
        primdata.PA_y[0] = Py - A[1];
#endif
#if LIBINT2_DEFINED(eri,PA_z)
        primdata.PA_z[0] = Pz - A[2];
#endif
#if LIBINT2_DEFINED(eri,AB_x)
        primdata.AB_x[0] = A[0] - B[0];
#endif
#if LIBINT2_DEFINED(eri,AB_y)
        primdata.AB_y[0] = A[1] - B[1];
#endif
#if LIBINT2_DEFINED(eri,AB_z)
        primdata.AB_z[0] = A[2] - B[2];
#endif
        }
        if (LIBINT2_SHELLQUARTET_SET != LIBINT2_SHELLQUARTET_SET_STANDARD
            || type_ == kinetic)
        { // always VRR on ket, HRR to ket (overlap, coulomb), or kinetic energy ints

#if LIBINT2_DEFINED(eri,PB_x)
        primdata.PB_x[0] = Px - B[0];
#endif
#if LIBINT2_DEFINED(eri,PB_y)
        primdata.PB_y[0] = Py - B[1];
#endif
#if LIBINT2_DEFINED(eri,PB_z)
        primdata.PB_z[0] = Pz - B[2];
#endif
#if LIBINT2_DEFINED(eri,BA_x)
        primdata.BA_x[0] = B[0] - A[0];
#endif
#if LIBINT2_DEFINED(eri,BA_y)
        primdata.BA_y[0] = B[1] - A[1];
#endif
#if LIBINT2_DEFINED(eri,BA_z)
        primdata.BA_z[0] = B[2] - A[2];
#endif
        }

#if LIBINT2_DEFINED(eri,oo2z)
        primdata.oo2z[0] = 0.5*oogammap;
#endif

        if (type_ == kinetic) { // additional factors for kinetic energy
#if LIBINT2_DEFINED(eri,rho12_over_alpha1)
        primdata.rho12_over_alpha1[0] = alpha2 * oogammap;
#endif
#if LIBINT2_DEFINED(eri,rho12_over_alpha2)
        primdata.rho12_over_alpha2[0] = alpha1 * oogammap;
#endif
#if LIBINT2_DEFINED(eri,two_rho12)
        primdata.two_rho12[0] = 2. * rhop;
#endif
        }

        if (type_ == nuclear) { // additional factor for electrostatic potential
          const auto& C = q_[oset].second;
#if LIBINT2_DEFINED(eri,PC_x)
        primdata.PC_x[0] = Px - C[0];
#endif
#if LIBINT2_DEFINED(eri,PC_y)
        primdata.PC_y[0] = Py - C[1];
#endif
#if LIBINT2_DEFINED(eri,PC_z)
        primdata.PC_z[0] = Pz - C[2];
#endif
        }

        if (deriv_order_ > 0) {
          // prefactors for derivative overlap relations
          assert(false);
        }

        const auto K1 = exp(- rhop * AB2) * oogammap;
        decltype(K1) sqrt_PI_cubed(5.56832799683170784528481798212);
        const auto ovlp_ss = sqrt_PI_cubed * sqrt(oogammap) * K1 * c1 * c2;

        primdata.LIBINT_T_S_OVERLAP_S[0] = ovlp_ss;

        if (type_ == kinetic) {
          primdata.LIBINT_T_S_KINETIC_S[0] = rhop * (3. - 2.*rhop*AB2) * ovlp_ss;
        }

        if (type_ == nuclear) {
#if LIBINT2_DEFINED(eri,PC_x) && LIBINT2_DEFINED(eri,PC_y) && LIBINT2_DEFINED(eri,PC_z)
          const auto PC2 = primdata.PC_x[0] * primdata.PC_x[0] +
                           primdata.PC_y[0] * primdata.PC_y[0] +
                           primdata.PC_z[0] * primdata.PC_z[0];
          const auto U = gammap * PC2;
          const auto ltot = s1.contr[0].l + s2.contr[0].l;
          double* fm_ptr = &(primdata.LIBINT_T_S_ELECPOT_S(0)[0]);
          fm_eval_->eval(fm_ptr, U, ltot);

          decltype(U) two_o_sqrt_PI(1.12837916709551257389615890312);
          const auto pfac = - q_[oset].first * sqrt(gammap) * two_o_sqrt_PI * ovlp_ss;
          const auto ltot_p1 = ltot + 1;
          for(auto m=0; m!=ltot_p1; ++m) {
            fm_ptr[m] *= pfac;
          }
#endif
        }

      }

    private:
      type type_;
      std::vector<Libint_t> primdata_;
      int lmax_;
      size_t deriv_order_;
      std::vector<std::pair<double, std::array<double,3>>> q_;

      std::shared_ptr<libint2::FmEval_Chebyshev3> fm_eval_;

      std::vector<LIBINT2_REALTYPE> scratch_; // for transposes and/or transforming to solid harmonics

      void initialize() {
        const auto ncart_max = (lmax_+1)*(lmax_+2)/2;

        switch(type_) {
          case overlap: assert(lmax_ <= LIBINT2_MAX_AM_overlap); break;
          case kinetic: assert(lmax_ <= LIBINT2_MAX_AM_kinetic); break;
          case nuclear: assert(lmax_ <= LIBINT2_MAX_AM_elecpot); break;
          default: assert(false);
        }
        assert(deriv_order_ <= LIBINT2_DERIV_ONEBODY_ORDER);

        if (type_ == overlap) {
          switch(deriv_order_) {

            case 0:
              libint2_init_overlap(&primdata_[0], lmax_, 0);
              scratch_.resize(ncart_max*ncart_max);
              break;
            case 1:
#if LIBINT2_DERIV_ONEBODY_ORDER > 0
              libint2_init_overlap1(&primdata_[0], lmax_, 0);
              scratch_.resize(3 * ncart_max*ncart_max);
#endif
              break;
            case 2:
#if LIBINT2_DERIV_ONEBODY_ORDER > 1
              libint2_init_overlap2(&primdata_[0], lmax_, 0);
              scratch_.resize(6 * ncart_max*ncart_max);
#endif
              break;
            default: assert(deriv_order_ < 3);
          }

          return;
        }

        if (type_ == kinetic) {
          switch(deriv_order_) {

            case 0:
              libint2_init_kinetic(&primdata_[0], lmax_, 0);
              scratch_.resize(ncart_max*ncart_max);
              break;
            case 1:
#if LIBINT2_DERIV_ONEBODY_ORDER > 0
              libint2_init_kinetic1(&primdata_[0], lmax_, 0);
              scratch_.resize(3 * ncart_max*ncart_max);
#endif
              break;
            case 2:
#if LIBINT2_DERIV_ONEBODY_ORDER > 1
              libint2_init_kinetic2(&primdata_[0], lmax_, 0);
              scratch_.resize(6 * ncart_max*ncart_max);
#endif
              break;
            default: assert(deriv_order_ < 3);
          }

          return;
        }

        if (type_ == nuclear) {

          switch(deriv_order_) {

            case 0:
              libint2_init_elecpot(&primdata_[0], lmax_, 0);
              scratch_.resize(ncart_max*ncart_max); // one more set to be able to accumulate
              break;
            case 1:
#if LIBINT2_DERIV_ONEBODY_ORDER > 0
              libint2_init_elecpot1(&primdata_[0], lmax_, 0);
              scratch_.resize(3 * ncart_max*ncart_max);
#endif
              break;
            case 2:
#if LIBINT2_DERIV_ONEBODY_ORDER > 1
              libint2_init_elecpot2(&primdata_[0], lmax_, 0);
              scratch_.resize(6 * ncart_max*ncart_max);
#endif
              break;
            default: assert(deriv_order_ < 3);
          }

          return;
        }

        assert(type_ == overlap || type_ == kinetic || type_ == nuclear);
      } // initialize()

      void finalize() {
        if (primdata_.size() != 0) {

          if (type_ == overlap) {
            switch(deriv_order_) {

              case 0:
              libint2_cleanup_overlap(&primdata_[0]);
              break;
              case 1:
  #if LIBINT2_DERIV_ONEBODY_ORDER > 0
              libint2_cleanup_overlap1(&primdata_[0]);
  #endif
              break;
              case 2:
  #if LIBINT2_DERIV_ONEBODY_ORDER > 1
              libint2_cleanup_overlap2(&primdata_[0]);
  #endif
              break;
            }

            return;
          }

          if (type_ == kinetic) {
            switch(deriv_order_) {

              case 0:
              libint2_cleanup_kinetic(&primdata_[0]);
              break;
              case 1:
  #if LIBINT2_DERIV_ONEBODY_ORDER > 0
              libint2_cleanup_kinetic1(&primdata_[0]);
  #endif
              break;
              case 2:
  #if LIBINT2_DERIV_ONEBODY_ORDER > 1
              libint2_cleanup_kinetic2(&primdata_[0]);
  #endif
              break;
            }

            return;
          }

          if (type_ == nuclear) {

            switch(deriv_order_) {

              case 0:
              libint2_cleanup_elecpot(&primdata_[0]);
              break;
              case 1:
  #if LIBINT2_DERIV_ONEBODY_ORDER > 0
              libint2_cleanup_elecpot1(&primdata_[0]);
  #endif
              break;
              case 2:
  #if LIBINT2_DERIV_ONEBODY_ORDER > 1
              libint2_cleanup_elecpot2(&primdata_[0]);
  #endif
              break;
            }

            return;
          }
        }

      } // finalize()


  }; // struct OneBodyEngine
#endif // LIBINT2_SUPPORT_ONEBODY

  /// types of multiplicative spherically-symmetric two-body kernels known by TwoBodyEngine
  enum MultiplicativeSphericalTwoBodyKernel {
    Coulomb,            //!< \f$ 1/r_{12} \f$
    cGTG,               //!< contracted Gaussian geminal = \f$ \sum_i c_i \exp(- \alpha r_{12}^2) \f$
    cGTG_times_Coulomb, //!< contracted Gaussian geminal times Coulomb
    DelcGTG_square      //!< (\f$ \nabla \f$ cGTG) \f$ \cdot \f$ (\f$ \nabla \f$ cGTG)
  };

  /// contracted Gaussian geminal = \f$ \sum_i c_i \exp(- \alpha r_{12}^2) \f$, represented as a vector of
  /// {\f$ \alpha_i \f$, \f$ c_i \f$ } pairs
  typedef std::vector<std::pair<double,double>> ContractedGaussianGeminal;

  namespace detail {
    template <int K> struct R12_K_G12_to_Kernel;
    template <> struct R12_K_G12_to_Kernel<-1> {
        static const MultiplicativeSphericalTwoBodyKernel value = cGTG_times_Coulomb;
    };
    template <> struct R12_K_G12_to_Kernel<0> {
        static const MultiplicativeSphericalTwoBodyKernel value = cGTG;
    };
    template <> struct R12_K_G12_to_Kernel<2> {
        static const MultiplicativeSphericalTwoBodyKernel value = DelcGTG_square;
    };

    template <MultiplicativeSphericalTwoBodyKernel Kernel> struct TwoBodyEngineDispatcher;

  } // namespace detail

  template <MultiplicativeSphericalTwoBodyKernel Kernel> struct TwoBodyEngineTraits;
  template <> struct TwoBodyEngineTraits<Coulomb> {
      typedef libint2::FmEval_Chebyshev3 core_eval_type;
      typedef struct {} oper_params_type;
  };
  template <> struct TwoBodyEngineTraits<cGTG> {
      typedef libint2::GaussianGmEval<LIBINT2_REALTYPE, 0> core_eval_type;
      typedef ContractedGaussianGeminal oper_params_type;
  };
  template <> struct TwoBodyEngineTraits<cGTG_times_Coulomb> {
      typedef libint2::GaussianGmEval<LIBINT2_REALTYPE, -1> core_eval_type;
      typedef ContractedGaussianGeminal oper_params_type;
  };
  template <> struct TwoBodyEngineTraits<DelcGTG_square> {
      typedef libint2::GaussianGmEval<LIBINT2_REALTYPE, 2> core_eval_type;
      typedef ContractedGaussianGeminal oper_params_type;
  };


#ifdef LIBINT2_SUPPORT_ERI
  /**
   * TwoBodyEngine computes (ab|O|cd) (i.e. <em>four-center</em>) integrals over
   * a two-body kernel of type MultiplicativeSphericalTwoBodyKernel using Obara-Saika-Ahlrichs relations.
   *
   * \tparam Kernel kernel type, the supported values are enumerated by MultiplicativeSphericalTwoBodyKernel
   */
  template <MultiplicativeSphericalTwoBodyKernel Kernel>
  class TwoBodyEngine {
    public:
      typedef typename libint2::TwoBodyEngineTraits<Kernel>::oper_params_type oper_params_type;

      /// creates a default (unusable) TwoBodyEngine
      TwoBodyEngine() : primdata_(), lmax_(-1), core_eval_(0) {}

      /// Constructs a (usable) TwoBodyEngine

      /// \param max_nprim the maximum number of primitives per contracted Gaussian shell
      /// \param max_l the maximum angular momentum of Gaussian shell
      /// \param deriv_level if not 0, will compute geometric derivatives of Gaussian integrals of order \c deriv_level
      /// \param oper_params specifies the operator parameters. The type of \c oper_params depends on \c Kernel as follows:
      ///        <ol>
      ///        <li> \c Coulomb : empty type (does not need to be provided) </li>
      ///        <li> \c cGTG : ContractedGaussianGeminal </li>
      ///        <li> \c cGTG_times_Coulomb : ContractedGaussianGeminal </li>
      ///        <li> \c DelcGTG_square : ContractedGaussianGeminal </li>
      ///        </ol>
      /// \warning currently only the following kernel types are suported: \c Coulomb
      /// \warning currently derivative integrals are not supported
      /// \warning currently solid harmonics Gaussians are not supported
      TwoBodyEngine(size_t max_nprim, int max_l, int deriv_order = 0, const oper_params_type& oparams = oper_params_type()) :
        primdata_(max_nprim * max_nprim * max_nprim * max_nprim), lmax_(max_l), deriv_order_(deriv_order),
        core_eval_(core_eval_type::instance(4*lmax_ + deriv_order, 1.e-15))
      {
        initialize();
        init_core_ints_params(oparams);
      }

      /// move constructor
      TwoBodyEngine(TwoBodyEngine&& other) = default;

      /// (deep) copy constructor
      TwoBodyEngine(const TwoBodyEngine& other) :
        primdata_(other.primdata_), lmax_(other.lmax_), deriv_order_(other.deriv_order_),
        core_eval_(other.core_eval_), core_ints_params_(other.core_ints_params_)
      {
        initialize();
      }

      ~TwoBodyEngine() {
        finalize();
      }

      /// move assignment
      TwoBodyEngine& operator=(TwoBodyEngine&& other) = default;

      /// (deep) copy assignment
      TwoBodyEngine& operator=(const TwoBodyEngine& other)
      {
        primdata_ = other.primdata_;
        lmax_ = other.lmax_;
        deriv_order_ = other.deriv_order_;
        core_eval_ = other.core_eval_;
        core_ints_params_ = other.core_ints_params_;
        initialize();
        return *this;
      }

      /// computes shell set of integrals
      /// \note result is stored in the "chemists" form, i.e. (tbra1 tbra2 |tket1 tket2), in row-major order
      const LIBINT2_REALTYPE* compute(const libint2::Shell& tbra1,
                                      const libint2::Shell& tbra2,
                                      const libint2::Shell& tket1,
                                      const libint2::Shell& tket2) {

        //
        // i.e. bra and ket refer to chemists bra and ket
        //

        // can only handle 1 contraction at a time
        assert(tbra1.ncontr() == 1 && tbra2.ncontr() == 1 &&
               tket1.ncontr() == 1 && tket2.ncontr() == 1);

        // derivatives not supported for now
        assert(deriv_order_ == 0);

#if LIBINT2_SHELLQUARTET_SET == LIBINT2_SHELLQUARTET_SET_STANDARD // standard angular momentum ordering
        auto swap_bra = (tbra1.contr[0].l < tbra2.contr[0].l);
        auto swap_ket = (tket1.contr[0].l < tket2.contr[0].l);
        auto swap_braket = (tbra1.contr[0].l + tbra2.contr[0].l > tket1.contr[0].l + tket2.contr[0].l);
#else // orca angular momentum ordering
        auto swap_bra = (tbra1.contr[0].l > tbra2.contr[0].l);
        auto swap_ket = (tket1.contr[0].l > tket2.contr[0].l);
        auto swap_braket = (tbra1.contr[0].l + tbra2.contr[0].l < tket1.contr[0].l + tket2.contr[0].l);
#endif
        const auto& bra1 = swap_braket ? (swap_ket ? tket2 : tket1) : (swap_bra ? tbra2 : tbra1);
        const auto& bra2 = swap_braket ? (swap_ket ? tket1 : tket2) : (swap_bra ? tbra1 : tbra2);
        const auto& ket1 = swap_braket ? (swap_bra ? tbra2 : tbra1) : (swap_ket ? tket2 : tket1);
        const auto& ket2 = swap_braket ? (swap_bra ? tbra1 : tbra2) : (swap_ket ? tket1 : tket2);

        const bool tform = tbra1.contr[0].pure || tbra2.contr[0].pure || tket1.contr[0].pure || tket2.contr[0].pure;
        const bool use_scratch = (swap_braket || swap_bra || swap_ket || tform);

        // assert # of primitive pairs
        auto nprim_bra1 = bra1.nprim();
        auto nprim_bra2 = bra2.nprim();
        auto nprim_ket1 = ket1.nprim();
        auto nprim_ket2 = ket2.nprim();

        // adjust max angular momentum, if needed
        auto lmax = std::max(std::max(bra1.contr[0].l, bra2.contr[0].l), std::max(ket1.contr[0].l, ket2.contr[0].l));
        assert (lmax <= lmax_);
        if (lmax == 0) // (ss|ss) ints will be accumulated in the first element of stack
          primdata_[0].stack[0] = 0;

        // compute primitive data
        {
          auto p = 0;
          for(auto pb1=0; pb1!=nprim_bra1; ++pb1) {
            for(auto pb2=0; pb2!=nprim_bra2; ++pb2) {
              for(auto pk1=0; pk1!=nprim_ket1; ++pk1) {
                for(auto pk2=0; pk2!=nprim_ket2; ++pk2, ++p) {
                  compute_primdata(primdata_[p],bra1,bra2,ket1,ket2,pb1,pb2,pk1,pk2);
                }
              }
            }
          }
          primdata_[0].contrdepth = p;
        }

        LIBINT2_REALTYPE* result = nullptr;

#ifdef FORCE_SOLID_TFORM_CHECK
        std::vector<LIBINT2_REALTYPE> cart_ints(tbra1.cartesian_size()*
                                                tbra2.cartesian_size()*
                                                tket1.cartesian_size()*
                                                tket2.cartesian_size());
#endif

        if (lmax == 0) { // (ss|ss)
          auto& stack = primdata_[0].stack[0];
          for(auto p=0; p != primdata_[0].contrdepth; ++p)
            stack += primdata_[p].LIBINT_T_SS_EREP_SS(0)[0];
          primdata_[0].targets[0] = primdata_[0].stack;

          result = primdata_[0].targets[0];
#ifdef FORCE_SOLID_TFORM_CHECK
          cart_ints[0] = result[0];
#endif
        }
        else { // not (ss|ss)
          LIBINT2_PREFIXED_NAME(libint2_build_eri)[bra1.contr[0].l][bra2.contr[0].l][ket1.contr[0].l][ket2.contr[0].l](&primdata_[0]);
          result = primdata_[0].targets[0];

          // if needed, permute (and transform ... soon :)
          if (use_scratch) {

#ifdef FORCE_SOLID_TFORM_CHECK
            {
              for(auto i1=0, i1234=0; i1<tbra1.cartesian_size(); ++i1) {
                for(auto i2=0; i2<tbra2.cartesian_size(); ++i2) {
                  for(auto i3=0; i3<tket1.cartesian_size(); ++i3) {
                    for(auto i4=0; i4<tket2.cartesian_size(); ++i4, ++i1234) {

                      const auto& j1 = swap_braket ? (swap_ket ? i4 : i3) : (swap_bra ? i2 : i1);
                      const auto& j2 = swap_braket ? (swap_ket ? i3 : i4) : (swap_bra ? i1 : i2);
                      const auto& j3 = swap_braket ? (swap_bra ? i2 : i1) : (swap_ket ? i4 : i3);
                      const auto& j4 = swap_braket ? (swap_bra ? i1 : i2) : (swap_ket ? i3 : i4);

                      cart_ints[i1234] = result[((j1*bra2.cartesian_size()+j2)*ket1.cartesian_size()+j3)*ket2.cartesian_size()+j4];
                    }
                  }
                }
              }

            }
#endif // FORCE_SOLID_TFORM_CHECK


            constexpr auto using_scalar_real = std::is_same<double,LIBINT2_REALTYPE>::value || std::is_same<float,LIBINT2_REALTYPE>::value;
            static_assert(using_scalar_real, "Libint2 C++11 API only supports fundamental real types");
            typedef Eigen::Matrix<LIBINT2_REALTYPE, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor > Matrix;

            // a 2-d view of the 4-d source tensor
            const auto nr1_cart = bra1.cartesian_size();
            const auto nr2_cart = bra2.cartesian_size();
            const auto nc1_cart = ket1.cartesian_size();
            const auto nc2_cart = ket2.cartesian_size();
            const auto ncol_cart = nc1_cart * nc2_cart;
            const auto nr1 = bra1.size();
            const auto nr2 = bra2.size();
            const auto nc1 = ket1.size();
            const auto nc2 = ket2.size();
            const auto nrow = nr1 * nr2;
            const auto ncol = nc1 * nc2;

            // a 2-d view of the 4-d target tensor
            const auto nr1_tgt = tbra1.size();
            const auto nr2_tgt = tbra2.size();
            const auto nc1_tgt = tket1.size();
            const auto nc2_tgt = tket2.size();
            const auto ncol_tgt = nc1_tgt * nc2_tgt;

            // transform to solid harmonics first, then unpermute, if necessary
            auto mainbuf = result;
            auto scratchbuf = &scratch_[0];
            if (bra1.contr[0].pure) {
              libint2::solidharmonics::transform_first(bra1.contr[0].l, nr2_cart*ncol_cart,
                                                       mainbuf, scratchbuf);
              std::swap(mainbuf, scratchbuf);
            }
            if (bra2.contr[0].pure) {
              libint2::solidharmonics::transform_inner(bra1.size(), bra2.contr[0].l, ncol_cart,
                                                       mainbuf, scratchbuf);
              std::swap(mainbuf, scratchbuf);
            }
            if (ket1.contr[0].pure) {
              libint2::solidharmonics::transform_inner(nrow, ket1.contr[0].l, nc2_cart,
                                                       mainbuf, scratchbuf);
              std::swap(mainbuf, scratchbuf);
            }
            if (ket2.contr[0].pure) {
              libint2::solidharmonics::transform_last(bra1.size()*bra2.size()*ket1.size(), ket2.contr[0].l,
                                                      mainbuf, scratchbuf);
              std::swap(mainbuf, scratchbuf);
            }

            // loop over rows of the source matrix
            const auto* src_row_ptr = mainbuf;
            auto tgt_ptr = scratchbuf;
            for(auto r1=0; r1!=nr1; ++r1) {
              for(auto r2=0; r2!=nr2; ++r2, src_row_ptr+=ncol) {

                typedef Eigen::Map<const Matrix> ConstMap;
                typedef Eigen::Map<Matrix> Map;
                typedef Eigen::Map<Matrix, Eigen::Unaligned, Eigen::Stride<Eigen::Dynamic,Eigen::Dynamic> > StridedMap;

                // represent this source row as a matrix
                ConstMap src_blk_mat(src_row_ptr, nc1, nc2);

                // and copy to the block of the target matrix
                if (swap_braket) {
                  // if swapped bra and ket, a row of source becomes a column of target
                  // source row {r1,r2} is mapped to target column {r1,r2} if !swap_ket, else to {r2,r1}
                  const auto tgt_col_idx = !swap_ket ? r1 * nr2 + r2 : r2 * nr1 + r1;
                  StridedMap tgt_blk_mat(tgt_ptr + tgt_col_idx, nr1_tgt, nr2_tgt,
                                         Eigen::Stride<Eigen::Dynamic,Eigen::Dynamic>(nr2_tgt*ncol_tgt,ncol_tgt));
                  if (swap_bra)
                    tgt_blk_mat = src_blk_mat.transpose();
                  else
                    tgt_blk_mat = src_blk_mat;
                }
                else {
                  // source row {r1,r2} is mapped to target row {r1,r2} if !swap_bra, else to {r2,r1}
                  const auto tgt_row_idx = !swap_bra ? r1 * nr2 + r2 : r2 * nr1 + r1;
                  Map tgt_blk_mat(tgt_ptr + tgt_row_idx*ncol, nc1_tgt, nc2_tgt);
                  if (swap_ket)
                    tgt_blk_mat = src_blk_mat.transpose();
                  else
                    tgt_blk_mat = src_blk_mat;
                }

              } // end of loop
            }   // over rows of source

            result = scratchbuf;

          } // if need_scratch => needed to transpose
#ifdef FORCE_SOLID_TFORM_CHECK
          else {
            std::copy(result, result+cart_ints.size(), cart_ints.begin());
          }
#endif

        } // not (ss|ss)

#ifdef FORCE_SOLID_TFORM_CHECK
        // validate tform by re-computing reference result here
        {
          if (tbra1.contr[0].pure && tbra2.contr[0].pure && tket1.contr[0].pure && tket2.contr[0].pure) {

            const auto n = tbra1.size() * tbra2.size() * tket1.size() * tket2.size();
            std::vector<LIBINT2_REALTYPE> ref_ints(n, 0.0);

            for(size_t s1=0, s1234=0; s1!=tbra1.size(); ++s1) {
              const solidharmonics::shg_coefs_type& coefs1 = solidharmonics::shg_coefs[tbra1.contr[0].l];
              const auto nc1 = coefs1.nnz(s1);      // # of cartesians contributing to shg s1
              const auto* c1_idxs = coefs1.row_idx(s1); // indices of cartesians contributing to shg s1
              const auto* c1_vals = coefs1.row_values(s1); // coefficients of cartesians contributing to shg s1

              for(size_t s2=0; s2!=tbra2.size(); ++s2) {
                const solidharmonics::shg_coefs_type& coefs2 = solidharmonics::shg_coefs[tbra2.contr[0].l];
                const auto nc2 = coefs2.nnz(s2);      // # of cartesians contributing to shg s1
                const auto* c2_idxs = coefs2.row_idx(s2); // indices of cartesians contributing to shg s1
                const auto* c2_vals = coefs2.row_values(s2); // coefficients of cartesians contributing to shg s1

                for(size_t s3=0; s3!=tket1.size(); ++s3) {
                  const solidharmonics::shg_coefs_type& coefs3 = solidharmonics::shg_coefs[tket1.contr[0].l];
                  const auto nc3 = coefs3.nnz(s3);      // # of cartesians contributing to shg s1
                  const auto* c3_idxs = coefs3.row_idx(s3); // indices of cartesians contributing to shg s1
                  const auto* c3_vals = coefs3.row_values(s3); // coefficients of cartesians contributing to shg s1

                  for(size_t s4=0; s4!=tket2.size(); ++s4, ++s1234) {
                    const solidharmonics::shg_coefs_type& coefs4 = solidharmonics::shg_coefs[tket2.contr[0].l];
                    const auto nc4 = coefs4.nnz(s4);      // # of cartesians contributing to shg s1
                    const auto* c4_idxs = coefs4.row_idx(s4); // indices of cartesians contributing to shg s1
                    const auto* c4_vals = coefs4.row_values(s4); // coefficients of cartesians contributing to shg s1

                    LIBINT2_REALTYPE tformed_value = 0.0;

                    for(size_t ic1=0; ic1!=nc1; ++ic1) { // loop over contributing cartesians
                      auto c1 = c1_idxs[ic1];
                      auto s1_c1_coeff = c1_vals[ic1];

                      for(size_t ic2=0; ic2!=nc2; ++ic2) { // loop over contributing cartesians
                        auto c2 = c2_idxs[ic2];
                        auto s2_c2_coeff = c2_vals[ic2];

                        for(size_t ic3=0; ic3!=nc3; ++ic3) { // loop over contributing cartesians
                          auto c3 = c3_idxs[ic3];
                          auto s3_c3_coeff = c3_vals[ic3];

                          for(size_t ic4=0; ic4!=nc4; ++ic4) { // loop over contributing cartesians
                            auto c4 = c4_idxs[ic4];
                            auto s4_c4_coeff = c4_vals[ic4];

                            tformed_value += s1_c1_coeff *
                                             s2_c2_coeff *
                                             s3_c3_coeff *
                                             s4_c4_coeff *
                                             cart_ints[((c1*tbra2.cartesian_size() + c2)*tket1.cartesian_size() + c3)*tket2.cartesian_size() + c4];
                          }
                        }
                      }
                    }

                    ref_ints[s1234] = tformed_value;
                  }
                }
              }
            }

            for(auto i=0; i<n; ++i) {
              if (::fabs(ref_ints[i] - result[i]) > 1e-12) {
                  throw "sanity test of solid tform failed!";
              }
            }

          }
        }
#endif // FORCE_SOLID_TFORM_CHECK


        return result;
      }

    private:

      void compute_primdata(Libint_t& primdata,
                            const Shell& sbra1,
                            const Shell& sbra2,
                            const Shell& sket1,
                            const Shell& sket2,
                            size_t pbra1,
                            size_t pbra2,
                            size_t pket1,
                            size_t pket2);

      std::vector<Libint_t> primdata_;
      int lmax_;
      size_t deriv_order_;

      typedef typename libint2::TwoBodyEngineTraits<Kernel>::core_eval_type core_eval_type;
      std::shared_ptr<core_eval_type> core_eval_;

      typedef oper_params_type core_ints_params_type; // currently core ints params are always same type as operator params
      core_ints_params_type core_ints_params_;
      /// converts operator parameters to core ints params
      void init_core_ints_params(const oper_params_type& oparams);

      std::vector<LIBINT2_REALTYPE> scratch_; // for transposes and/or transforming to solid harmonics

      friend struct detail::TwoBodyEngineDispatcher<Kernel>;

      void initialize() {
        const auto ncart_max = (lmax_+1)*(lmax_+2)/2;
        const auto max_shellpair_size = ncart_max * ncart_max;
        const auto max_shellset_size = max_shellpair_size * max_shellpair_size;

        assert(lmax_ <= LIBINT2_MAX_AM_ERI);
        assert(deriv_order_ <= LIBINT2_DERIV_ONEBODY_ORDER);

        switch(deriv_order_) {

            case 0:
              libint2_init_eri(&primdata_[0], lmax_, 0);
              scratch_.resize(max_shellset_size);
              break;
            case 1:
#if LIBINT2_DERIV_ERI_ORDER > 0
              libint2_init_eri1(&primdata_[0], lmax_, 0);
              scratch_.resize(9 * max_shellset_size);
#endif
              break;
            case 2:
#if LIBINT2_DERIV_ERI_ORDER > 1
              libint2_init_eri2(&primdata_[0], lmax_, 0);
              scratch_.resize(45 * max_shellset_size);
#endif
              break;
            default: assert(deriv_order_ < 3);
        }
      }

      void finalize() {
        if (primdata_.size() != 0) {
          switch(deriv_order_) {

            case 0:
              libint2_cleanup_eri(&primdata_[0]);
              break;
            case 1:
#if LIBINT2_DERIV_ERI_ORDER > 0
              libint2_cleanup_eri1(&primdata_[0]);
#endif
              break;
            case 2:
#if LIBINT2_DERIV_ERI_ORDER > 1
              libint2_cleanup_eri2(&primdata_[0]);
#endif
              break;
          }
        }
      }

  }; // struct TwoBodyEngine

  namespace detail {
    template <> struct TwoBodyEngineDispatcher<Coulomb> {
        static void core_eval(TwoBodyEngine<Coulomb>* engine,
                              LIBINT2_REALTYPE* Fm,
                              int mmax,
                              LIBINT2_REALTYPE T,
                              LIBINT2_REALTYPE) {
          engine->core_eval_->eval(Fm, T, mmax);
        }
    };

    template <>
    struct TwoBodyEngineDispatcher<cGTG_times_Coulomb> {
        static void core_eval(TwoBodyEngine<cGTG_times_Coulomb>* engine,
                              LIBINT2_REALTYPE* Gm,
                              int mmax,
                              LIBINT2_REALTYPE T,
                              LIBINT2_REALTYPE rho) {
          engine->core_eval_->eval(Gm, rho, T, mmax, engine->core_ints_params_);
        }
    };
    template <>
    struct TwoBodyEngineDispatcher<cGTG> {
        static void core_eval(TwoBodyEngine<cGTG>* engine,
                              LIBINT2_REALTYPE* Gm,
                              int mmax,
                              LIBINT2_REALTYPE T,
                              LIBINT2_REALTYPE rho) {
          engine->core_eval_->eval(Gm, rho, T, mmax, engine->core_ints_params_);
        }
    };
    template <>
    struct TwoBodyEngineDispatcher<DelcGTG_square> {
        static void core_eval(TwoBodyEngine<DelcGTG_square>* engine,
                              LIBINT2_REALTYPE* Gm,
                              int mmax,
                              LIBINT2_REALTYPE T,
                              LIBINT2_REALTYPE rho) {
          engine->core_eval_->eval(Gm, rho, T, mmax, engine->core_ints_params_);
        }
    };
  }

  template <MultiplicativeSphericalTwoBodyKernel Kernel>
  void TwoBodyEngine<Kernel>::compute_primdata(Libint_t& primdata,
                        const Shell& sbra1,
                        const Shell& sbra2,
                        const Shell& sket1,
                        const Shell& sket2,
                        size_t pbra1,
                        size_t pbra2,
                        size_t pket1,
                        size_t pket2) {

    const auto& A = sbra1.O;
    const auto& B = sbra2.O;
    const auto& C = sket1.O;
    const auto& D = sket2.O;

    const auto alpha0 = sbra1.alpha[pbra1];
    const auto alpha1 = sbra2.alpha[pbra2];
    const auto alpha2 = sket1.alpha[pket1];
    const auto alpha3 = sket2.alpha[pket2];

    const auto c0 = sbra1.contr[0].coeff[pbra1];
    const auto c1 = sbra2.contr[0].coeff[pbra2];
    const auto c2 = sket1.contr[0].coeff[pket1];
    const auto c3 = sket2.contr[0].coeff[pket2];

    const auto amtot = sbra1.contr[0].l + sket1.contr[0].l +
                       sbra2.contr[0].l + sket2.contr[0].l;

    const auto gammap = alpha0 + alpha1;
    const auto oogammap = 1.0 / gammap;
    const auto rhop = alpha0 * alpha1 * oogammap;
    const auto Px = (alpha0 * A[0] + alpha1 * B[0]) * oogammap;
    const auto Py = (alpha0 * A[1] + alpha1 * B[1]) * oogammap;
    const auto Pz = (alpha0 * A[2] + alpha1 * B[2]) * oogammap;
    const auto AB_x = A[0] - B[0];
    const auto AB_y = A[1] - B[1];
    const auto AB_z = A[2] - B[2];
    const auto AB2 = AB_x*AB_x + AB_y*AB_y + AB_z*AB_z;

    const auto gammaq = alpha2 + alpha3;
    const auto oogammaq = 1.0 / gammaq;
    const auto rhoq = alpha2 * alpha3 * oogammaq;
    const auto gammapq = gammap + gammaq;
    const auto sqrt_gammapq = sqrt(gammapq);
    const auto oogammapq = 1.0 / (gammapq);
    const auto rho = gammap * gammaq * oogammapq;
    const auto Qx = (alpha2 * C[0] + alpha3 * D[0]) * oogammaq;
    const auto Qy = (alpha2 * C[1] + alpha3 * D[1]) * oogammaq;
    const auto Qz = (alpha2 * C[2] + alpha3 * D[2]) * oogammaq;
    const auto CD_x = C[0] - D[0];
    const auto CD_y = C[1] - D[1];
    const auto CD_z = C[2] - D[2];
    const auto CD2 = CD_x * CD_x + CD_y * CD_y + CD_z * CD_z;

    const auto PQx = Px - Qx;
    const auto PQy = Py - Qy;
    const auto PQz = Pz - Qz;
    const auto PQ2 = PQx * PQx + PQy * PQy + PQz * PQz;

    const auto K1 = exp(- rhop * AB2);
    const auto K2 = exp(- rhoq * CD2);
    decltype(K1) two_times_M_PI_to_25(34.986836655249725693); // (2 \pi)^{5/2}
    double pfac = two_times_M_PI_to_25 * K1 * K2  * oogammap * oogammaq * sqrt_gammapq * oogammapq;
    pfac *= c0 * c1 * c2 * c3;

    const auto T = PQ2*rho;
    double* fm_ptr = &(primdata.LIBINT_T_SS_EREP_SS(0)[0]);
    const auto mmax = amtot + deriv_order_;

//        timers.stop(0);
//        timers.start(3);
        //core_eval_->eval(fm_ptr, T, mmax);
    detail::TwoBodyEngineDispatcher<Kernel>::core_eval(this, fm_ptr, mmax, T, rho);
//        timers.stop(3);
//        timers.start(0);
//        timers.start(4);
//        timers.stop(4);

    for(auto m=0; m!=mmax+1; ++m) {
      fm_ptr[m] *= pfac;
    }

    if (mmax == 0)
      return;

#if LIBINT2_DEFINED(eri,PA_x)
    primdata.PA_x[0] = Px - A[0];
#endif
#if LIBINT2_DEFINED(eri,PA_y)
    primdata.PA_y[0] = Py - A[1];
#endif
#if LIBINT2_DEFINED(eri,PA_z)
    primdata.PA_z[0] = Pz - A[2];
#endif
#if LIBINT2_DEFINED(eri,PB_x)
    primdata.PB_x[0] = Px - B[0];
#endif
#if LIBINT2_DEFINED(eri,PB_y)
    primdata.PB_y[0] = Py - B[1];
#endif
#if LIBINT2_DEFINED(eri,PB_z)
    primdata.PB_z[0] = Pz - B[2];
#endif

#if LIBINT2_DEFINED(eri,QC_x)
    primdata.QC_x[0] = Qx - C[0];
#endif
#if LIBINT2_DEFINED(eri,QC_y)
    primdata.QC_y[0] = Qy - C[1];
#endif
#if LIBINT2_DEFINED(eri,QC_z)
    primdata.QC_z[0] = Qz - C[2];
#endif
#if LIBINT2_DEFINED(eri,QD_x)
    primdata.QD_x[0] = Qx - D[0];
#endif
#if LIBINT2_DEFINED(eri,QD_y)
    primdata.QD_y[0] = Qy - D[1];
#endif
#if LIBINT2_DEFINED(eri,QD_z)
    primdata.QD_z[0] = Qz - D[2];
#endif

#if LIBINT2_DEFINED(eri,AB_x)
    primdata.AB_x[0] = AB_x;
#endif
#if LIBINT2_DEFINED(eri,AB_y)
    primdata.AB_y[0] = AB_y;
#endif
#if LIBINT2_DEFINED(eri,AB_z)
    primdata.AB_z[0] = AB_z;
#endif
#if LIBINT2_DEFINED(eri,BA_x)
    primdata.BA_x[0] = -AB_x;
#endif
#if LIBINT2_DEFINED(eri,BA_y)
    primdata.BA_y[0] = -AB_y;
#endif
#if LIBINT2_DEFINED(eri,BA_z)
    primdata.BA_z[0] = -AB_z;
#endif

#if LIBINT2_DEFINED(eri,CD_x)
    primdata.CD_x[0] = CD_x;
#endif
#if LIBINT2_DEFINED(eri,CD_y)
    primdata.CD_y[0] = CD_y;
#endif
#if LIBINT2_DEFINED(eri,CD_z)
    primdata.CD_z[0] = CD_z;
#endif
#if LIBINT2_DEFINED(eri,DC_x)
    primdata.DC_x[0] = -CD_x;
#endif
#if LIBINT2_DEFINED(eri,DC_y)
    primdata.DC_y[0] = -CD_y;
#endif
#if LIBINT2_DEFINED(eri,DC_z)
    primdata.DC_z[0] = -CD_z;
#endif

    const auto gammap_o_gammapgammaq = oogammapq * gammap;
    const auto gammaq_o_gammapgammaq = oogammapq * gammaq;

    const auto Wx = (gammap_o_gammapgammaq * Px + gammaq_o_gammapgammaq * Qx);
    const auto Wy = (gammap_o_gammapgammaq * Py + gammaq_o_gammapgammaq * Qy);
    const auto Wz = (gammap_o_gammapgammaq * Pz + gammaq_o_gammapgammaq * Qz);

#if LIBINT2_DEFINED(eri,WP_x)
    primdata.WP_x[0] = Wx - Px;
#endif
#if LIBINT2_DEFINED(eri,WP_y)
    primdata.WP_y[0] = Wy - Py;
#endif
#if LIBINT2_DEFINED(eri,WP_z)
    primdata.WP_z[0] = Wz - Pz;
#endif
#if LIBINT2_DEFINED(eri,WQ_x)
    primdata.WQ_x[0] = Wx - Qx;
#endif
#if LIBINT2_DEFINED(eri,WQ_y)
    primdata.WQ_y[0] = Wy - Qy;
#endif
#if LIBINT2_DEFINED(eri,WQ_z)
    primdata.WQ_z[0] = Wz - Qz;
#endif
#if LIBINT2_DEFINED(eri,oo2z)
    primdata.oo2z[0] = 0.5*oogammap;
#endif
#if LIBINT2_DEFINED(eri,oo2e)
    primdata.oo2e[0] = 0.5*oogammaq;
#endif
#if LIBINT2_DEFINED(eri,oo2ze)
    primdata.oo2ze[0] = 0.5*oogammapq;
#endif
#if LIBINT2_DEFINED(eri,roz)
    primdata.roz[0] = rho*oogammap;
#endif
#if LIBINT2_DEFINED(eri,roe)
    primdata.roe[0] = rho*oogammaq;
#endif

    // using ITR?
#if LIBINT2_DEFINED(eri,TwoPRepITR_pfac0_0_0_x)
    Data->TwoPRepITR_pfac0_0_0_x[0] = - (alpha1*AB_x + alpha3*CD_x) * oogammap;
#endif
#if LIBINT2_DEFINED(eri,TwoPRepITR_pfac0_0_0_y)
    Data->TwoPRepITR_pfac0_0_0_y[0] = - (alpha1*AB_y + alpha3*CD_y) * oogammap;
#endif
#if LIBINT2_DEFINED(eri,TwoPRepITR_pfac0_0_0_z)
    Data->TwoPRepITR_pfac0_0_0_z[0] = - (alpha1*AB_z + alpha3*CD_z) * oogammap;
#endif
#if LIBINT2_DEFINED(eri,TwoPRepITR_pfac0_1_0_x)
    Data->TwoPRepITR_pfac0_1_0_x[0] = - (alpha1*AB_x + alpha3*CD_x) * oogammaq;
#endif
#if LIBINT2_DEFINED(eri,TwoPRepITR_pfac0_1_0_y)
    Data->TwoPRepITR_pfac0_1_0_y[0] = - (alpha1*AB_y + alpha3*CD_y) * oogammaq;
#endif
#if LIBINT2_DEFINED(eri,TwoPRepITR_pfac0_1_0_z)
    Data->TwoPRepITR_pfac0_1_0_z[0] = - (alpha1*AB_z + alpha3*CD_z) * oogammaq;
#endif
#if LIBINT2_DEFINED(eri,TwoPRepITR_pfac0_0_1_x)
    Data->TwoPRepITR_pfac0_0_1_x[0] = (alpha0*AB_x + alpha2*CD_x) * oogammap;
#endif
#if LIBINT2_DEFINED(eri,TwoPRepITR_pfac0_0_1_y)
    Data->TwoPRepITR_pfac0_0_1_y[0] = (alpha0*AB_y + alpha2*CD_y) * oogammap;
#endif
#if LIBINT2_DEFINED(eri,TwoPRepITR_pfac0_0_1_z)
    Data->TwoPRepITR_pfac0_0_1_z[0] = (alpha0*AB_z + alpha2*CD_z) * oogammap;
#endif
#if LIBINT2_DEFINED(eri,TwoPRepITR_pfac0_1_1_x)
    Data->TwoPRepITR_pfac0_1_1_x[0] = (alpha0*AB_x + alpha2*CD_x) * oogammaq;
#endif
#if LIBINT2_DEFINED(eri,TwoPRepITR_pfac0_1_1_y)
    Data->TwoPRepITR_pfac0_1_1_y[0] = (alpha0*AB_y + alpha2*CD_y) * oogammaq;
#endif
#if LIBINT2_DEFINED(eri,TwoPRepITR_pfac0_1_1_z)
    Data->TwoPRepITR_pfac0_1_1_z[0] = (alpha0*AB_z + alpha2*CD_z) * oogammaq;
#endif

    // prefactors for derivative ERI relations
    if (deriv_order_ > 0) {
#if LIBINT2_DEFINED(eri,alpha1_rho_over_zeta2)
      primdata.alpha1_rho_over_zeta2[0] = alpha0 * rho / (gammap * gammap);
#endif
#if LIBINT2_DEFINED(eri,alpha2_rho_over_zeta2)
      primdata.alpha2_rho_over_zeta2[0] = alpha1 * rho / (gammap * gammap);
#endif
#if LIBINT2_DEFINED(eri,alpha3_rho_over_eta2)
      primdata.alpha3_rho_over_eta2[0] = alpha2 * rho / (gammaq * gammaq);
#endif
#if LIBINT2_DEFINED(eri,alpha4_rho_over_eta2)
      primdata.alpha4_rho_over_eta2[0] = alpha3 * rho / (gammaq * gammaq);
#endif
#if LIBINT2_DEFINED(eri,alpha1_over_zetapluseta)
      primdata.alpha1_over_zetapluseta[0] = alpha0 / (gammap + gammaq);
#endif
#if LIBINT2_DEFINED(eri,alpha2_over_zetapluseta)
      primdata.alpha2_over_zetapluseta[0] = alpha1 / (gammap + gammaq);
#endif
#if LIBINT2_DEFINED(eri,alpha3_over_zetapluseta)
      primdata.alpha3_over_zetapluseta[0] = alpha2 / (gammap + gammaq);
#endif
#if LIBINT2_DEFINED(eri,alpha4_over_zetapluseta)
      primdata.alpha4_over_zetapluseta[0] = alpha3 / (gammap + gammaq);
#endif
#if LIBINT2_DEFINED(eri,rho12_over_alpha1)
      primdata.rho12_over_alpha1[0] = rhop / alpha0;
#endif
#if LIBINT2_DEFINED(eri,rho12_over_alpha2)
      primdata.rho12_over_alpha2[0] = rhop / alpha1;
#endif
#if LIBINT2_DEFINED(eri,rho34_over_alpha3)
      primdata.rho34_over_alpha3[0] = rhoq / alpha2;
#endif
#if LIBINT2_DEFINED(eri,rho34_over_alpha4)
      primdata.rho34_over_alpha4[0] = rhoq / alpha3;
#endif
#if LIBINT2_DEFINED(eri,two_alpha0_bra)
      primdata.two_alpha0_bra[0] = 2.0 * alpha0;
#endif
#if LIBINT2_DEFINED(eri,two_alpha0_ket)
      primdata.two_alpha0_ket[0] = 2.0 * alpha1;
#endif
#if LIBINT2_DEFINED(eri,two_alpha1_bra)
      primdata.two_alpha1_bra[0] = 2.0 * alpha2;
#endif
#if LIBINT2_DEFINED(eri,two_alpha1_ket)
      primdata.two_alpha1_ket[0] = 2.0 * alpha3;
#endif
    }
  }


  template <>
  inline void TwoBodyEngine<DelcGTG_square>::init_core_ints_params(
      const oper_params_type& oparams) {
    // [g12,[- \Del^2, g12] = 2 (\Del g12) \cdot (\Del g12)
    // (\Del exp(-a r_12^2) \cdot (\Del exp(-b r_12^2) = 4 a b (r_{12}^2 exp(- (a+b) r_{12}^2) )
    // i.e. need to scale each coefficient by 4 a b
    const auto ng = oparams.size();
    core_ints_params_.reserve(ng*(ng+1)/2);
    for(size_t b=0; b<ng; ++b)
      for(size_t k=0; k<=b; ++k) {
        const auto gexp = oparams[b].first + oparams[k].first;
        const auto gcoeff = oparams[b].second * oparams[k].second * (b == k ? 1 : 2); // if a != b include ab and ba
        const auto gcoeff_rescaled = 4 * oparams[b].first * oparams[k].first * gcoeff;
        core_ints_params_.push_back(std::make_pair(gexp, gcoeff_rescaled));
      }
  }

  template <MultiplicativeSphericalTwoBodyKernel Kernel>
  inline void TwoBodyEngine<Kernel>::init_core_ints_params(
      const oper_params_type& oparams) {
    core_ints_params_ = oparams;
  }


#endif // LIBINT2_SUPPORT_ERI

} // namespace libint2

#endif /* _libint2_src_lib_libint_engine_h_ */
