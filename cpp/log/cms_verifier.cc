/* -*- indent-tabs-mode: nil -*- */
#include "log/cms_verifier.h"
#include "log/ct_extensions.h"

using std::string;
using std::unique_ptr;
using util::Status;
using util::StatusOr;

namespace cert_trans {
util::StatusOr<bool> CmsVerifier::IsCmsSignedByCert(BIO* cms_bio_in,
                                                     const Cert& cert) const {
  CHECK_NOTNULL(cms_bio_in);

  if (!cert.IsLoaded()) {
    LOG(ERROR) << "Can't check cert signer as it's not loaded";
    return Status(util::error::FAILED_PRECONDITION, "Cert not loaded");
  }

  CMS_ContentInfo* const cms_content_info = d2i_CMS_bio(cms_bio_in, nullptr);

  if (!cms_content_info) {
    LOG(ERROR) << "Could not parse CMS data";
    LOG_OPENSSL_ERRORS(WARNING);
    return Status(util::error::INVALID_ARGUMENT,
                  "CMS data could not be parsed");
  }

  // This stack must not be freed as it points into the CMS structure
  STACK_OF(CMS_SignerInfo) *
      const signers(CMS_get0_SignerInfos(cms_content_info));

  if (signers) {
    for (int s = 0; s < sk_CMS_SignerInfo_num(signers); ++s) {
      CMS_SignerInfo* const signer = sk_CMS_SignerInfo_value(signers, s);

      if (CMS_SignerInfo_cert_cmp(signer, cert.x509_) == 0) {
        CMS_ContentInfo_free(cms_content_info);
        return true;
      }
    }
  }

  CMS_ContentInfo_free(cms_content_info);
  return false;
}

StatusOr<bool> CmsVerifier::IsCmsSignedByCert(const string& cms_object,
                                              const Cert* cert) const {
  CHECK_NOTNULL(cert);

  if (!cert->IsLoaded()) {
    LOG(ERROR) << "Can't check cert signer as it's not loaded";
    return Status(util::error::FAILED_PRECONDITION, "Cert not loaded");
  }

  // Load a source bio with the CMS signed data object and parse it
  BIO* const source_bio = BIO_new(BIO_s_mem());
  BIO_write(source_bio, cms_object.c_str(), cms_object.length());

  CMS_ContentInfo* const cms_content_info = d2i_CMS_bio(source_bio, nullptr);
  BIO_free(source_bio);

  if (!cms_content_info) {
    LOG(ERROR) << "Could not parse CMS data";
    LOG_OPENSSL_ERRORS(WARNING);
    return Status(util::error::INVALID_ARGUMENT,
                  "CMS data could not be parsed");
  }

  // Now that we've got the CMS unpacked check it has a valid signature using
  // the same key as the cert. First create a certificate stack from our
  // expected signing cert that can be used by CMS_verify.
  STACK_OF(X509)* validation_chain = sk_X509_new(nullptr);

  sk_X509_push(validation_chain, cert->x509_);

  // Must set CMS_NOINTERN as the RFC says certs SHOULD be omitted from the
  // message but the client might not have obeyed this. CMS_BINARY is required
  // to avoid MIME-related translation. CMS_NO_SIGNER_CERT_VERIFY because we
  // will do our own checks that the chain is valid and the message may not
  // be signed directly by a trusted cert. We don't check it's a signed data
  // object CMS type as OpenSSL does this.
  const int verified =
      CMS_verify(cms_content_info, validation_chain, nullptr, nullptr, nullptr,
                 CMS_NO_SIGNER_CERT_VERIFY | CMS_NOINTERN | CMS_BINARY);

  sk_X509_free(validation_chain);

  if (verified != 1) {
    // Most likely, was not CMS signed by the precert
    CMS_ContentInfo_free(cms_content_info);
    return false;
  }

  // This stack must not be freed as it points into the CMS structure
  STACK_OF(CMS_SignerInfo) *
      const signers(CMS_get0_SignerInfos(cms_content_info));

  if (signers) {
    for (int s = 0; s < sk_CMS_SignerInfo_num(signers); ++s) {
      CMS_SignerInfo* const signer = sk_CMS_SignerInfo_value(signers, s);

      if (CMS_SignerInfo_cert_cmp(signer, cert->x509_) == 0) {
        CMS_ContentInfo_free(cms_content_info);
        return true;
      }
    }
  }

  CMS_ContentInfo_free(cms_content_info);
  return false;
}


util::Status CmsVerifier::UnpackCmsDerBio(BIO* cms_bio_in, const Cert& cert,
                                           BIO* cms_bio_out) {
  CHECK_NOTNULL(cms_bio_in);

  if (!cert.IsLoaded()) {
    LOG(ERROR) << "Cert for CMS verify not loaded";
    return Status(util::error::FAILED_PRECONDITION, "Cert not loaded");
  }

  CMS_ContentInfo* const cms_content_info = d2i_CMS_bio(cms_bio_in, nullptr);

  if (!cms_content_info) {
    LOG(ERROR) << "Could not parse CMS data";
    LOG_OPENSSL_ERRORS(WARNING);
    return Status(util::error::INVALID_ARGUMENT,
                  "CMS data could not be parsed");
  }

  const ASN1_OBJECT* message_content_type(
      CMS_get0_eContentType(cms_content_info));
  int content_type_nid = OBJ_obj2nid(message_content_type);
  // TODO: Enforce content type here. This is not yet defined in the RFC.
  if (content_type_nid != NID_ctV2CmsPayloadContentType) {
    LOG(WARNING) << "CMS message content has unexpected type: "
                 << content_type_nid;
  }

  // Create a certificate stack from our expected signing cert that can be used
  // by CMS_verify.
  STACK_OF(X509)* validation_chain = sk_X509_new(nullptr);

  sk_X509_push(validation_chain, cert.x509_);

  // Must set CMS_NOINTERN as the RFC says certs SHOULD be omitted from the
  // message but the client might not have obeyed this. CMS_BINARY is required
  // to avoid MIME-related translation. CMS_NO_SIGNER_CERT_VERIFY because we
  // will do our own checks that the chain is valid and the message may not
  // be signed directly by a trusted cert. We don't check it's a signed data
  // object CMS type as OpenSSL does this.
  int verified = CMS_verify(cms_content_info, validation_chain, nullptr,
                            nullptr, cms_bio_out,
                            CMS_NO_SIGNER_CERT_VERIFY | CMS_NOINTERN
                            | CMS_BINARY);

  sk_X509_free(validation_chain);

  CMS_ContentInfo_free(cms_content_info);

  return (verified == 1) ? util::Status::OK
                         : util::Status(util::error::INVALID_ARGUMENT,
                                        "CMS verification failed");
}


util::Status CmsVerifier::UnpackCmsDerBio(BIO* cms_bio_in, BIO* cms_bio_out) {
  CHECK_NOTNULL(cms_bio_in);
  CHECK_NOTNULL(cms_bio_out);

  CMS_ContentInfo* const cms_content_info = d2i_CMS_bio(cms_bio_in, nullptr);

  if (!cms_content_info) {
    LOG(ERROR) << "Could not parse CMS data";
    LOG_OPENSSL_ERRORS(WARNING);
    return Status(util::error::INVALID_ARGUMENT,
                  "CMS data could not be parsed");
  }

  const ASN1_OBJECT* message_content_type(
      CMS_get0_eContentType(cms_content_info));
  const int content_type_nid = OBJ_obj2nid(message_content_type);
  // TODO: Enforce content type here. This is not yet defined in the RFC.
  if (content_type_nid != NID_ctV2CmsPayloadContentType) {
    LOG(WARNING) << "CMS message content has unexpected type: "
                 << content_type_nid;
  }

  // Must set CMS_NOINTERN as the RFC says certs SHOULD be omitted from the
  // message but the client might not have obeyed this. CMS_BINARY is required
  // to avoid MIME-related translation. CMS_NO_SIGNER_CERT_VERIFY because we
  // will do our own checks that the chain is valid and the message may not
  // be signed directly by a trusted cert. CMS_NO_CONTENT_VERIFY because we
  // can't apply the RFC mandated signature checks until we have the unpacked
  // cert to examine. We don't check it's a signed data object CMS type as
  // OpenSSL does this.
  const int verified =
      CMS_verify(cms_content_info, nullptr, nullptr, nullptr, cms_bio_out,
                 CMS_NO_SIGNER_CERT_VERIFY | CMS_NOINTERN | CMS_BINARY |
                     CMS_NO_CONTENT_VERIFY);

  CMS_ContentInfo_free(cms_content_info);

  return (verified == 1) ? util::Status::OK
                         : util::Status(util::error::INVALID_ARGUMENT,
                                        "CMS unpack failed");
}


Cert* CmsVerifier::UnpackCmsSignedCertificate(BIO* cms_bio_in,
                                               const Cert& verify_cert) {
  CHECK_NOTNULL(cms_bio_in);
  BIO* unpacked_bio = BIO_new(BIO_s_mem());
  unique_ptr<Cert> cert(new Cert());

  if (UnpackCmsDerBio(cms_bio_in, verify_cert, unpacked_bio).ok()) {
    // The unpacked data should be a valid DER certificate.
    // TODO: The RFC does not yet define this as the format so this may
    // need to change.
    util::Status status = cert->LoadFromDerBio(unpacked_bio);

    if (!status.ok()) {
      LOG(WARNING) << "Could not unpack cert from CMS DER encoded data";
    }
  } else {
    LOG_OPENSSL_ERRORS(ERROR);
  }

  BIO_free(unpacked_bio);

  return cert.release();
}

Cert* CmsVerifier::UnpackCmsSignedCertificate(const string& cms_object) {
  // Load the source bio with the CMS signed data object
  BIO* source_bio = BIO_new(BIO_s_mem());
  BIO_write(source_bio, cms_object.c_str(), cms_object.length());

  BIO* unpacked_bio = BIO_new(BIO_s_mem());
  unique_ptr<Cert> cert(new Cert);

  if (UnpackCmsDerBio(source_bio, unpacked_bio).ok()) {
    // The unpacked data should be a valid DER certificate.
    // TODO: The RFC does not yet define this as the format so this may
    // need to change.
    const Status status = cert->LoadFromDerBio(unpacked_bio);

    if (!status.ok()) {
      LOG(WARNING) << "Could not unpack cert from CMS DER encoded data";
    }
  } else {
    LOG_OPENSSL_ERRORS(ERROR);
  }

  BIO_free(source_bio);
  BIO_free(unpacked_bio);

  return cert.release();
}

}  // namespace cert_trans
