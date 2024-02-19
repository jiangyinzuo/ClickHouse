#include <Common/SipHash.h>
#include <Common/FieldVisitorToString.h>
#include <Common/FieldVisitorHash.h>
#include <Parsers/ASTLiteral.h>
#include <IO/WriteHelpers.h>
#include <IO/WriteBufferFromString.h>
#include <IO/Operators.h>


namespace DB
{

void ASTLiteral::updateTreeHashImpl(SipHash & hash_state) const
{
    const char * prefix = "Literal_";
    hash_state.update(prefix, strlen(prefix));
    applyVisitor(FieldVisitorHash(hash_state), value);
}

ASTPtr ASTLiteral::clone() const
{
    auto res = std::make_shared<ASTLiteral>(*this);
    res->unique_column_name = {};
    return res;
}

namespace
{

/// Writes 'tuple' word before tuple literals for backward compatibility reasons.
class FieldVisitorToColumnName : public StaticVisitor<String>
{
public:
    template<typename T>
    String operator() (const T & x) const { return visitor(x); }

private:
    FieldVisitorToString visitor;
};

template<>
String FieldVisitorToColumnName::operator() (const Tuple & x) const
{
    WriteBufferFromOwnString wb;

    wb << "tuple(";
    for (auto it = x.begin(); it != x.end(); ++it)
    {
        if (it != x.begin())
            wb << ", ";
        wb << applyVisitor(*this, *it);
    }
    wb << ')';

    return wb.str();
}

}

void ASTLiteral::appendColumnNameImpl(WriteBuffer & ostr) const
{
    if (use_legacy_column_name)
    {
        appendColumnNameImplLegacy(ostr);
        return;
    }

    /// 100 - just an arbitrary value.
    constexpr auto min_elements_for_hashing = 100;

    /// Special case for very large arrays and tuples. Instead of listing all elements, we will use a hash of them.
    /// (Otherwise, the column name will be too long, which will lead to a significant slowdown of expression analysis.)
    /// Also, for aggregate functions, we should include the type name to distinguish the states of different types.
    auto type = value.getType();
    if ((type == Field::Types::Array && value.get<const Array &>().size() > min_elements_for_hashing)
        || (type == Field::Types::Tuple && value.get<const Tuple &>().size() > min_elements_for_hashing)
        || (type == Field::Types::AggregateFunctionState))
    {
        SipHash hash;
        applyVisitor(FieldVisitorHash(hash), value);
        UInt64 low, high;
        hash.get128(low, high);

        const char * prefix = "";
        if (type == Field::Types::Array)
            prefix = "__array_";
        else if (type == Field::Types::Tuple)
            prefix = "__tuple_";
        else if (type == Field::Types::AggregateFunctionState)
            prefix = "__aggregate_function_";
        
        writeCString(prefix, ostr);
        writeText(low, ostr);
        ostr.write('_');
        writeText(high, ostr);
    }
    else
    {
        /// Shortcut for huge AST. The `FieldVisitorToString` becomes expensive
        /// for tons of literals as it creates a temporary String.
        if (value.getType() == Field::Types::String)
        {
            writeQuoted(value.get<String>(), ostr);
        }
        else
        {
            String column_name = applyVisitor(FieldVisitorToString(), value);
            writeString(column_name, ostr);
        }
    }
}

void ASTLiteral::appendColumnNameImplLegacy(WriteBuffer & ostr) const
{
    /// 100 - just an arbitrary value.
    constexpr auto min_elements_for_hashing = 100;

    /// Special case for very large arrays. Instead of listing all elements, we will use a hash of them.
    /// (Otherwise, the column name will be too long, which will lead to a significant slowdown of expression analysis.)
    auto type = value.getType();
    if ((type == Field::Types::Array && value.get<const Array &>().size() > min_elements_for_hashing))
    {
        SipHash hash;
        applyVisitor(FieldVisitorHash(hash), value);
        UInt64 low, high;
        hash.get128(low, high);

        writeCString("__array_", ostr);
        writeText(low, ostr);
        ostr.write('_');
        writeText(high, ostr);
    }
    else
    {
        String column_name = applyVisitor(FieldVisitorToColumnName(), value);
        writeString(column_name, ostr);
    }
}

/// Use different rules for escaping backslashes and quotes
class FieldVisitorToStringPostgreSQL : public StaticVisitor<String>
{
public:
    template<typename T>
    String operator() (const T & x) const { return visitor(x); }

private:
    FieldVisitorToString visitor;
};

template<>
String FieldVisitorToStringPostgreSQL::operator() (const String & x) const
{
    WriteBufferFromOwnString wb;
    writeQuotedStringPostgreSQL(x, wb);
    return wb.str();
}

void ASTLiteral::formatImplWithoutAlias(const FormatSettings & settings, IAST::FormatState &, IAST::FormatStateStacked) const
{
    if (settings.literal_escaping_style == LiteralEscapingStyle::Regular)
        settings.ostr << applyVisitor(FieldVisitorToString(), value);
    else
        settings.ostr << applyVisitor(FieldVisitorToStringPostgreSQL(), value);
}

}
