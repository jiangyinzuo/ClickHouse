#include <Analyzer/Passes/IfTransformStringsToEnumPass.h>

#include <Analyzer/ConstantNode.h>
#include <Analyzer/FunctionNode.h>
#include <Analyzer/IQueryTreeNode.h>
#include <Analyzer/InDepthQueryTreeVisitor.h>
#include <Analyzer/Utils.h>

#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeEnum.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/IDataType.h>

#include <Functions/FunctionFactory.h>

namespace DB
{

namespace
{

/// We place strings in ascending order here under the assumption it could speed up String to Enum conversion.
template <typename EnumType>
auto getDataEnumType(const std::set<std::string> & string_values)
{
    using EnumValues = typename EnumType::Values;
    EnumValues enum_values;
    enum_values.reserve(string_values.size());

    size_t number = 1;
    for (const auto & value : string_values)
        enum_values.emplace_back(value, number++);

    return std::make_shared<EnumType>(std::move(enum_values));
}

DataTypePtr getEnumType(const std::set<std::string> & string_values)
{
    if (string_values.size() >= 255)
        return getDataEnumType<DataTypeEnum16>(string_values);
    else
        return getDataEnumType<DataTypeEnum8>(string_values);
}

/// if(arg1, arg2, arg3) will be transformed to if(arg1, _CAST(arg2, Enum...), _CAST(arg3, Enum...))
/// where Enum is generated based on the possible values stored in string_values
void changeIfArguments(
    FunctionNode & if_node, const std::set<std::string> & string_values, const ContextPtr & context)
{
    auto result_type = getEnumType(string_values);

    auto & argument_nodes = if_node.getArguments().getNodes();

    argument_nodes[1] = createCastFunction(argument_nodes[1], result_type, context);
    argument_nodes[2] = createCastFunction(argument_nodes[2], result_type, context);

    auto if_resolver = FunctionFactory::instance().get("if", context);

    if_node.resolveAsFunction(if_resolver->build(if_node.getArgumentColumns()));
}

/// transform(value, array_from, array_to, default_value) will be transformed to transform(value, array_from, _CAST(array_to, Array(Enum...)), _CAST(default_value, Enum...))
/// where Enum is generated based on the possible values stored in string_values
void changeTransformArguments(
    FunctionNode & transform_node,
    const std::set<std::string> & string_values,
    const ContextPtr & context)
{
    auto result_type = getEnumType(string_values);

    auto & arguments = transform_node.getArguments().getNodes();

    auto & array_to = arguments[2];
    auto & default_value = arguments[3];

    array_to = createCastFunction(array_to, std::make_shared<DataTypeArray>(result_type), context);
    default_value = createCastFunction(default_value, std::move(result_type), context);

    auto transform_resolver = FunctionFactory::instance().get("transform", context);

    transform_node.resolveAsFunction(transform_resolver->build(transform_node.getArgumentColumns()));
}

/// Find functions that are used inside the other functions
struct FindUsedFunctionsVisitor : public InDepthQueryTreeVisitorWithContext<FindUsedFunctionsVisitor>
{
public:
    using Base = InDepthQueryTreeVisitorWithContext<FindUsedFunctionsVisitor>;
    using Base::Base;

    std::unordered_set<QueryTreeNodePtrWithHash> used_functions;

    void enterImpl(QueryTreeNodePtr & node)
    {
        auto * function_node = node->as<FunctionNode>();
        if (!function_node)
            return;

        std::string_view function_name = function_node->getFunctionName();

        if ((function_name == "if" || function_name == "transform")
            && function_scope_count > 0)
        {
            used_functions.insert(node);
        }

        ++function_scope_count;
    }

    void leaveImpl(QueryTreeNodePtr & node)
    {
        if (node->getNodeType() == QueryTreeNodeType::FUNCTION)
            --function_scope_count;
    }

private:
    size_t function_scope_count = 0;
};

class ConvertStringsToEnumVisitor : public InDepthQueryTreeVisitorWithContext<ConvertStringsToEnumVisitor>
{
public:
    using Base = InDepthQueryTreeVisitorWithContext<ConvertStringsToEnumVisitor>;
    using Base::Base;

    ConvertStringsToEnumVisitor(std::unordered_set<QueryTreeNodePtrWithHash> used_functions_, ContextPtr context)
        : Base(std::move(context))
        , used_functions(std::move(used_functions_))
    {}

    void enterImpl(QueryTreeNodePtr & node)
    {
        if (!getSettings().optimize_if_transform_strings_to_enum)
            return;

        auto * function_node = node->as<FunctionNode>();
        if (!function_node)
            return;

        const auto & context = getContext();

        /// to preserve return type (String) of the current function_node, we wrap the newly
        /// generated function nodes into toString

        std::string_view function_name = function_node->getFunctionName();
        if (function_name == "if")
        {
            if (used_functions.contains(node) || function_node->getArguments().getNodes().size() != 3)
                return;

            auto & argument_nodes = function_node->getArguments().getNodes();

            const auto * first_literal = argument_nodes[1]->as<ConstantNode>();
            const auto * second_literal = argument_nodes[2]->as<ConstantNode>();

            if (!first_literal || !second_literal)
                return;

            if (!isString(first_literal->getResultType()) || !isString(second_literal->getResultType()))
                return;

            std::set<std::string> string_values;
            string_values.insert(first_literal->getValue().get<std::string>());
            string_values.insert(second_literal->getValue().get<std::string>());

            changeIfArguments(*function_node, string_values, context);
            return;
        }

        if (function_name == "transform")
        {
            if (used_functions.contains(node) || function_node->getArguments().getNodes().size() != 4)
                return;

            auto & argument_nodes = function_node->getArguments().getNodes();

            if (!isString(function_node->getResultType()))
                return;

            const auto * literal_to = argument_nodes[2]->as<ConstantNode>();
            const auto * literal_default = argument_nodes[3]->as<ConstantNode>();

            if (!literal_to || !literal_default)
                return;

            if (!isArray(literal_to->getResultType()) || !isString(literal_default->getResultType()))
                return;

            auto array_to = literal_to->getValue().get<Array>();

            if (array_to.empty())
                return;

            if (!std::all_of(
                    array_to.begin(),
                    array_to.end(),
                    [](const auto & field) { return field.getType() == Field::Types::Which::String; }))
                return;

            /// collect possible string values
            std::set<std::string> string_values;

            for (const auto & value : array_to)
                string_values.insert(value.get<std::string>());

            string_values.insert(literal_default->getValue().get<std::string>());

            changeTransformArguments(*function_node, string_values, context);
            return;
        }
    }
private:
    std::unordered_set<QueryTreeNodePtrWithHash> used_functions;
};

}

void IfTransformStringsToEnumPass::run(QueryTreeNodePtr & query, ContextPtr context)
{
    if (!context->getSettingsRef().optimize_if_transform_strings_to_enum)
        return;

    /// first we need to find all if/transform functions used in other functions
    /// we cannot modify them because they need to keep same type
    FindUsedFunctionsVisitor used_functions_visitor(context);
    used_functions_visitor.visit(query);

    ConvertStringsToEnumVisitor visitor(std::move(used_functions_visitor.used_functions), std::move(context));
    visitor.visit(query);
}

}
