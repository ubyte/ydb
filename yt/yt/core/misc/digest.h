#pragma once

#include "public.h"

#include <yt/yt/core/phoenix/context.h>
#include <yt/yt/core/phoenix/type_decl.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

//! This class holds a compact representation of a set of samples.
//! #IDigest::GetQuantile|(alpha)| returns a lower bound |X| such that the number
//! of samples less than |X| is no less than |alpha|.
// TODO(max42): add methods GetCDF(X) -> alpha (inverse to GetQuantile).
// TODO(max42): add support for serialization/deserialization.
// TODO(max42): implement Q-Digest (https://github.com/addthis/stream-lib/blob/master/src/main/java/com/clearspring/analytics/stream/quantile/QDigest.java)
// and T-Digest (https://github.com/tdunning/t-digest) algorithms and compare them with TLogDigest.
struct IDigest
    : public TRefCounted
{
    virtual void AddSample(double value) = 0;

    virtual double GetQuantile(double alpha) const = 0;

    virtual void Reset() = 0;
};

DEFINE_REFCOUNTED_TYPE(IDigest)

////////////////////////////////////////////////////////////////////////////////

struct IPersistentDigest
    : public IDigest
    , public NPhoenix2::IPersistent
{
    using TLoadContext = NPhoenix2::TLoadContext;
    using TSaveContext = NPhoenix2::TSaveContext;

    PHOENIX_DECLARE_POLYMORPHIC_TYPE(IPersistentDigest, 0x1ed99609);
};

DEFINE_REFCOUNTED_TYPE(IPersistentDigest)

////////////////////////////////////////////////////////////////////////////////

IPersistentDigestPtr CreateLogDigest(TLogDigestConfigPtr config);

////////////////////////////////////////////////////////////////////////////////

IDigestPtr CreateHistogramDigest(THistogramDigestConfigPtr config);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
