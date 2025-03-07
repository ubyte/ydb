#pragma once

#include "public.h"

#include <yt/yt/core/yson/string.h>

#include <yt/yt/core/ytree/public.h>

#include <vector>

namespace NYT::NSignature {

////////////////////////////////////////////////////////////////////////////////

class TSignature final
{
public:
    // NB(pavook) only needed for Deserialize internals.

    //! Constructs an empty TSignature.
    TSignature() = default;

    //! Creates a TSignature containing the given payload without an actual signature.
    explicit TSignature(NYson::TYsonString payload);

    [[nodiscard]] const NYson::TYsonString& Payload() const;

private:
    // TODO(arkady-e1ppa): Whenever trivial generator/validators are added
    // remove initialization.
    NYson::TYsonString Header_ = NYson::TYsonString(TStringBuf(""));
    NYson::TYsonString Payload_;
    std::vector<std::byte> Signature_;

    friend class ISignatureGenerator;
    friend class ISignatureValidator;

    friend void Serialize(const TSignature& signature, NYson::IYsonConsumer* consumer);
    friend void Deserialize(TSignature& signature, NYTree::INodePtr node);
    friend void Deserialize(TSignature& signature, NYson::TYsonPullParserCursor* cursor);
};

DEFINE_REFCOUNTED_TYPE(TSignature)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSignature
