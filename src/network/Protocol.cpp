#include "Protocol.hpp"

#include <cstring>

Expression protocol::expressionFromString(const char* text) {
    if (!text || !*text) return Expression::Neutral;
    for (uint8_t i = 0; i < kExpressionCount; ++i) {
        const auto e = static_cast<Expression>(i);
        if (!strcmp(text, expressionName(e))) return e;
    }
    return Expression::Neutral;
}

const char* protocol::expressionToString(Expression e) {
    return expressionName(e);
}
