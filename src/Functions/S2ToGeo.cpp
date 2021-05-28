#include "config_functions.h"

#include <Columns/ColumnsNumber.h>
#include <Columns/ColumnTuple.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeTuple.h>
#include <Functions/FunctionFactory.h>
#include <Functions/IFunctionImpl.h>
#include <Common/typeid_cast.h>
#include <ext/range.h>

#include "../contrib/s2geometry/src/s2/s2latlng.h"
#include "../contrib/s2geometry/src/s2/s2cell_id.h"

class S2CellId;

namespace DB 
{

namespace ErrorCodes
{
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

namespace
{

/// TODO: Comment this
class FunctionS2ToGeo : public IFunction
{
public:
    static constexpr auto name = "S2ToGeo";

    static FunctionPtr create(ContextPtr)
    {
        return std::make_shared<FunctionS2ToGeo>();
    }

    std::string getName() const override
    {
        return name;
    }

    size_t getNumberOfArguments() const override { return 1; }

    bool useDefaultImplementationForConstants() const override { return true; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        size_t number_of_arguments = arguments.size();

        if (number_of_arguments != 1) {
            throw Exception("Number of arguments for function " + getName() + " doesn't match: passed "
                + toString(number_of_arguments) + ", should be 2",
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);
        }

        const auto * arg = arguments[0].get();

        if (!WhichDataType(arg).isUInt64()) {
            throw Exception(
                "Illegal type " + arg->getName() + " of argument " + std::to_string(1) + " of function " + getName() + ". Must be Float64",
                ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
        }

        DataTypePtr element = std::make_shared<DataTypeUInt64>();

        return std::make_shared<DataTypeTuple>(DataTypes{element, element});
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t input_rows_count) const override
    {
        const auto * col_id = arguments[0].column.get();

        auto col_res_first = ColumnUInt32::create();
        auto col_res_second = ColumnUInt32::create();

        auto & vec_res_first = col_res_first->getData();
        vec_res_first.resize(input_rows_count);

        auto & vec_res_second = col_res_second->getData();
        vec_res_second.resize(input_rows_count);

        for (const auto row : ext::range(0, input_rows_count))
        {
            const UInt64 id = col_id->getUInt(row);

            S2CellId cell_id(id);
            S2Point point = cell_id.ToPoint();
            S2LatLng ll(point);
            
            vec_res_first[row] = ll.lng().degrees();
            vec_res_second[row] = ll.lat().degrees();
        }

        return ColumnTuple::create(Columns{std::move(col_res_first), std::move(col_res_second)});
    }

};

}

void registerFunctionGeoToS2(FunctionFactory & factory)
{
    factory.registerFunction<FunctionS2ToGeo>();
}


}
