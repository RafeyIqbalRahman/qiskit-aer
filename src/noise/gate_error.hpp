/**
 * Copyright 2018, IBM.
 *
 * This source code is licensed under the Apache License, Version 2.0 found in
 * the LICENSE.txt file in the root directory of this source tree.
 */

/**
 * @file    gate_error.hpp
 * @brief   Gate Error class for Qiskit-Aer simulator
 * @author  Christopher J. Wood <cjwood@us.ibm.com>
 */

#ifndef _aer_noise_gate_error_hpp_
#define _aer_noise_gate_error_hpp_

#include "noise/unitary_error.hpp"
#include "noise/kraus_error.hpp"

// TODO: add support for diagonal matrices

namespace AER {
namespace Noise {

//=========================================================================
// Gate Error class
//=========================================================================

// This combines unitary and Kraus errors into one error class to prevent
// the inefficient use of unitary matrices in a Kraus decomposition.

class GateError : public Error {
public:

  GateError() = default;
  GateError(const std::vector<cmatrix_t> &mats) {set_from_ops(mats, 1.0);}

  //-----------------------------------------------------------------------
  // Error base required methods
  //-----------------------------------------------------------------------

  // Sample a noisy implementation of op
  NoiseOps sample_noise(const Operations::Op &op,
                        const reg_t &qubits,
                        RngEngine &rng) override;

  //-----------------------------------------------------------------------
  // Additional class methods
  //-----------------------------------------------------------------------

  // Construct a gate error from a vector of Kraus matrices for a CPTP map
  // This will automatically partition the operators into unitary and Kraus
  // errors based on the type of operators. The optional probability parameter
  // is the error probability (default 1).
  void set_from_ops(const std::vector<cmatrix_t> &mats, double p_error = 1);

  void set_probabilities(double p_identity, double p_unitary, double p_kraus);

  void set_kraus(const KrausError &err);

  void set_unitary(const UnitaryError &err);

protected:
  // Probability of noise type:
  // 0 -> No error
  // 1 -> UnitaryError error
  // 2 -> Kraus error
  std::discrete_distribution<uint_t> probabilities_;

  // Sub-error classes
  UnitaryError unitary_error_;
  KrausError kraus_error_;
};


//-----------------------------------------------------------------------
// Implementation
//-----------------------------------------------------------------------

NoiseOps GateError::sample_noise(const Operations::Op &op,
                                 const reg_t &qubits,
                                 RngEngine &rng) {

  auto noise_type = rng.rand_int(probabilities_);
  switch (noise_type) {
    case 0:
      return {op};
    case 1:
      return unitary_error_.sample_noise(op, qubits, rng);
    case 2:
      return kraus_error_.sample_noise(op, qubits, rng);
    default:
      // We shouldn't get here, but just in case...
      throw std::invalid_argument("GateError type is out of range.");
  }
};

void GateError::set_probabilities(double p_identity, double p_unitary, double p_kraus) {
  probabilities_ = std::discrete_distribution<uint_t>({p_identity, p_unitary, p_kraus});
}

void GateError::set_kraus(const KrausError &err) {
  kraus_error_ = err;
}

void GateError::set_unitary(const UnitaryError &err) {
  unitary_error_ = err;
}

void GateError::set_from_ops(const std::vector<cmatrix_t> &mats,
                             double p_error) {

  double threshold = 1e-10; // move this to argument?

  // Check input is a CPTP map
  cmatrix_t cptp(mats[0].size());
  for (const auto &mat : mats) {
    cptp = cptp + Utils::dagger(mat) * mat;
  }
  if (Utils::is_identity(cptp, threshold) == false) {
    throw std::invalid_argument("GateError input is not a CPTP map.");
  }

  // Check if each matrix is a scaled identity, scaled unitary, or general
  // kraus operator

  double p_identity = 0;
  double p_unitary = 0.;
  double p_kraus = 0.;

  rvector_t probs_unitaries;
  std::vector<cmatrix_t> unitaries;
  std::vector<cmatrix_t> kraus;

  for (const auto &mat : mats) {
    // TODO: add support for diagonal matrices
    if (!Utils::is_square(mat)) {
      throw std::invalid_argument("Error matrix is not square.");
    }
    // Get the (0, 0) value of mat * dagger(mat) for rescaling
    double p = 0.;
    for (size_t j=0; j < mat.GetRows(); j ++)
      p += std::real(std::abs(mat(j, 0) * std::conj(mat(0, j))));
    if (p > 0) {
      // rescale mat by probability
      cmatrix_t tmp = (1 / std::sqrt(p)) * mat;
      // check if rescaled matrix is an identity
      if (Utils::is_identity(tmp, threshold)) {
        p_identity += p;
      }
      // check if rescaled matrix is a unitary
      else if (Utils::is_unitary(tmp, threshold)) {
        unitaries.push_back(tmp);
        probs_unitaries.push_back(p);
        p_unitary += p;
      } else {
        // Original matrix is non-unitary so add it to Kraus ops
        kraus.push_back(mat);
      }
    }
  }
  // Infer probability of the kraus error from other terms.
  p_kraus = 1. - p_identity - p_unitary;
  // sanity check:
  if (std::abs(p_identity + p_unitary + p_kraus - 1) > threshold) {
    throw std::invalid_argument("GateError deduced probabilities invalid.");
  }

  // Now we rescale probabilities to into account the p_error parameter
  // rescale Kraus operators
  if (p_kraus > 0 && p_kraus < 1)
    for (auto &k : kraus)
      k = (1 / std::sqrt(p_kraus)) * k;
  // Rescale unitary probabilities
  if (p_unitary > 0 && p_unitary < 1)
    for (auto &p : probs_unitaries)
      p = p / p_unitary;

  // Set the gate error probabilities
  set_probabilities(1 - p_error + p_error * p_identity,
                    p_error * p_unitary,
                    p_error * p_kraus);

  // Set the gate error operators
  unitary_error_.set_probabilities(probs_unitaries);
  unitary_error_.set_unitaries(unitaries);
  kraus_error_.set_kraus(kraus);
  if (kraus.empty())
    kraus_error_.set_probability(0);
  else
    kraus_error_.set_probability(1);
  
  // TODO validate the noise model
}

/* Schema idea?

{
  "type": "gate_error",   // string
  "operations": ["x", "y", "z"], // list string
  "qubits": [[0, 1], [1], [2], [3]], // qubits gate applies to
  "targets": null, // if null targets are qubits
  "params": [mat1, mat2, mat3]
}

{
  "type": "reset_error";
  "operations": ["x", "y", "z"], // list string
  "qubits": null, // if null or missing all qubits same
  "targets": null, // if null or missing targets are input qubits
  "params": ???
}

*/

//-------------------------------------------------------------------------
} // end namespace Noise
//-------------------------------------------------------------------------
} // end namespace AER
//-------------------------------------------------------------------------
#endif