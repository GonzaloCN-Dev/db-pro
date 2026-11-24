#include "core.h"

#include <cstdio>

namespace dbp {

std::string hex_preview(const std::string &b, size_t max)
{
    static const char *H = "0123456789ABCDEF";
    std::string r = "0x";
    for (size_t k = 0; k < b.size() && k < max; ++k) {
        unsigned char c = (unsigned char)b[k];
        r += H[c >> 4];
        r += H[c & 15];
    }
    if (b.size() > max) r += "… (" + std::to_string(b.size()) + " bytes)";
    return r;
}

Value Value::from_text_for(TypeClass c, std::string t)
{
    switch (c) {
    case TypeClass::Decimal: return of(Kind::Decimal, std::move(t));
    case TypeClass::Date:
    case TypeClass::Time:
    case TypeClass::DateTime: return of(Kind::Temporal, std::move(t));
    case TypeClass::Uuid: return of(Kind::Uuid, std::move(t));
    case TypeClass::Json: return of(Kind::Json, std::move(t));
    case TypeClass::ObjectId: return of(Kind::ObjectId, std::move(t));
    default: return text(std::move(t));
    }
}

std::string Value::display() const
{
    switch (kind) {
    case Kind::Null: return "NULL";
    case Kind::Missing: return "";
    case Kind::Default: return "DEFAULT";
    case Kind::Bool: return i ? "true" : "false";
    case Kind::Int: return std::to_string(i);
    case Kind::Float: {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.17g", d);
        // Prefer the shortest representation that round-trips.
        for (int p = 6; p <= 17; ++p) {
            char t[40];
            std::snprintf(t, sizeof t, "%.*g", p, d);
            double back = 0;
            if (std::sscanf(t, "%lf", &back) == 1 && back == d) {
                std::snprintf(buf, sizeof buf, "%s", t);
                break;
            }
        }
        return buf;
    }
    case Kind::Bytes: return hex_preview(s);
    default: return s;
    }
}

std::string Value::edit_text() const
{
    switch (kind) {
    case Kind::Null:
    case Kind::Missing:
    case Kind::Default: return "";
    case Kind::Bytes: return display();
    default: return display();
    }
}

int op_arity(Op op)
{
    switch (op) {
    case Op::IsEmpty: case Op::NotEmpty: case Op::IsNull: case Op::NotNull:
    case Op::IsTrue: case Op::IsFalse: case Op::Exists: case Op::NotExists: return 0;
    case Op::Between: return 2;
    case Op::In: case Op::NotIn: return -1;
    default: return 1;
    }
}

const char *op_label(Op op)
{
    switch (op) {
    case Op::Contains: return "contains";
    case Op::NotContains: return "does not contain";
    case Op::Equals: return "equals";
    case Op::NotEquals: return "not equal";
    case Op::StartsWith: return "starts with";
    case Op::EndsWith: return "ends with";
    case Op::IsEmpty: return "is empty";
    case Op::NotEmpty: return "is not empty";
    case Op::Regex: return "regex";
    case Op::Gt: return ">";
    case Op::Lt: return "<";
    case Op::Gte: return ">=";
    case Op::Lte: return "<=";
    case Op::Between: return "between";
    case Op::In: return "in";
    case Op::NotIn: return "not in";
    case Op::IsNull: return "is NULL";
    case Op::NotNull: return "is not NULL";
    case Op::IsTrue: return "is true";
    case Op::IsFalse: return "is false";
    case Op::Exists: return "exists";
    case Op::NotExists: return "does not exist";
    }
    return "";
}

} // namespace dbp
