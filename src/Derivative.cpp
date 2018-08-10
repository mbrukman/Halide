#include "Derivative.h"

#include "BoundaryConditions.h"
#include "DerivativeUtils.h"
#include "Error.h"
#include "FindCalls.h"
#include "IREquality.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "RealizationOrder.h"
#include "Simplify.h"
#include "Substitute.h"

#include <cmath>
#include <iostream>

namespace Halide {
namespace Internal {

bool check_opname(const std::string &op_name,
                  const std::string &func_name) {
    return op_name == (func_name + "_f16") ||
           op_name == (func_name + "_f32") ||
           op_name == (func_name + "_f64");
};

/** Compute derivatives through reverse accumulation
 */
class ReverseAccumulationVisitor : public IRVisitor {
public:
    using IRVisitor::visit;

    void propagate_adjoints(const Func &output,
                            const Func &adjoint,
                            const std::vector<std::pair<Expr, Expr>> &output_bounds);

    std::map<FuncKey, Func> get_adjoint_funcs() const {
        return adjoint_funcs;
    }

protected:
    void visit(const Cast *op);
    void visit(const Variable *op);
    void visit(const Add *op);
    void visit(const Sub *op);
    void visit(const Mul *op);
    void visit(const Div *op);
    void visit(const Min *op);
    void visit(const Max *op);
    void visit(const Let *op);
    void visit(const Select *op);
    void visit(const Call *op);

private:
    void accumulate(const Expr &stub, const Expr &adjoint);

    // For each expression, we store the accumulated adjoints expression
    std::map<const BaseExprNode *, Expr> expr_adjoints;
    // For each function and each update, we store the accumulated adjoints func
    std::map<FuncKey, Func> adjoint_funcs;
    // Let variables and their mapping
    std::map<std::string, Expr> let_var_mapping;
    std::vector<std::string> let_variables;
    // Bounds of functions
    std::map<std::string, Box> func_bounds;
    // Current function that scatters its adjoints to its dependencies
    Func current_func;
    // Current update of the function
    int current_update_id;
};

void ReverseAccumulationVisitor::propagate_adjoints(
    const Func &output,
    const Func &adjoint,
    const std::vector<std::pair<Expr, Expr>> &output_bounds) {
    // Topologically sort the functions
    std::map<std::string, Function> env = find_transitive_calls(output.function());
    std::vector<std::string> order =
        realization_order({ output.function() }, env).first;
    std::vector<Func> funcs;
    funcs.reserve(order.size());
    // Internal::debug(0) << "Sorted Func list:" << "\n";
    // for (const auto &func_name : order) {
    //     Internal::debug(0) << "  . " << func_name << "\n";
    // }
    for (const auto &func_name : order) {
        Func func(env[func_name]);
        funcs.push_back(Func(env[func_name]));
    }

    internal_assert(funcs.size() > 0);
    func_bounds = inference_bounds(output, output_bounds);

    // Create a stub for each function to accumulate adjoints.
    for (int func_id = 0; func_id < (int) funcs.size(); func_id++) {
        const Func &func = funcs[func_id];
        for (int update_id = -1;
             update_id < func.num_update_definitions(); update_id++) {
            Func adjoint_func(
                func.name() + "_" + std::to_string(update_id + 1) + "_d_def__");
            bool is_final_output = func_id == (int) funcs.size() - 1 &&
                                   update_id == func.num_update_definitions() - 1;
            std::vector<Var> args = func.args();
            for (auto &arg : args) {
                if (arg.is_implicit()) {
                    // Replace implicit variables with non implicit ones
                    arg = Var();
                }
            }
            if (is_final_output) {
                adjoint_func(args) = adjoint(args);
            } else {
                if (func.values().size() == 1) {
                    adjoint_func(args) = make_const(func.values()[0].type(), 0.0);
                } else {
                    std::vector<Expr> init(func.values().size());
                    for (int i = 0; i < (int) init.size(); i++) {
                        init[i] = make_const(func.values()[i].type(), 0.0);
                    }
                    adjoint_func(args) = Tuple(init);
                }
            }
            FuncKey func_key{ func.name(), update_id };
            assert(adjoint_funcs.find(func_key) == adjoint_funcs.end());
            adjoint_funcs[func_key] = adjoint_func;
        }
    }
    // Also create stubs for buffers referenced by the functions
    std::map<std::string, BufferInfo> called_buffers;
    for (int func_id = 0; func_id < (int) funcs.size(); func_id++) {
        const Func &func = funcs[func_id];
        std::map<std::string, BufferInfo> buffers = find_buffer_calls(func);
        called_buffers.insert(buffers.begin(), buffers.end());
    }
    for (const auto &it : called_buffers) {
        Func adjoint_func(it.first + "_d__");
        std::vector<Var> args;
        for (int i = 0; i < it.second.dimension; i++) {
            args.push_back(Var());
        }
        adjoint_func(args) = make_const(it.second.type, 0.0);
        FuncKey func_key{ it.first, -1 };
        if (adjoint_funcs.find(func_key) != adjoint_funcs.end()) {
            user_error << "Naming conflict between buffer and function:" << it.first << "\n";
        }
        adjoint_funcs[func_key] = adjoint_func;
    }

    // Traverse functions from producers to consumers for reverse accumulation
    for (int func_id = funcs.size() - 1; func_id >= 0; func_id--) {
        const Func &func = funcs[func_id];
        current_func = func;

        // Traverse from the last update to first
        for (int update_id = func.num_update_definitions() - 1;
             update_id >= -1; update_id--) {
            current_update_id = update_id;
            FuncKey func_key{ func.name(), update_id };
            internal_assert(func_bounds.find(func.name()) != func_bounds.end());

            // Set up boundary condition if this is the first visit to the function
            if (update_id == func.num_update_definitions() - 1 &&
                func.dimensions() > 0) {
                Func &adjoint_func = adjoint_funcs[func_key];
                const Box &bounds = func_bounds[func.name()];

                // Save a pointer to the unbounded def. Useful for scheduling
                FuncKey unbounded_func_key{ func.name() + "_unbounded", update_id };
                adjoint_funcs[unbounded_func_key] = adjoint_func;

                if (adjoint_func.values().size() == 1) {
                    Type type = adjoint_func.values()[0].type();
                    adjoint_func = BoundaryConditions::constant_exterior(adjoint_func, make_zero(type), box_to_vector(bounds));
                } else {
                    std::vector<Expr> values(adjoint_func.values().size());
                    for (int i = 0; i < (int) values.size(); i++) {
                        values[i] = make_const(adjoint_func.values()[i].type(), 0.0);
                    }
                    adjoint_func = BoundaryConditions::constant_exterior(adjoint_func, Tuple(values), box_to_vector(bounds));
                }
            }

            // Initialize the next adjoint function by propagating the adjoints to next update
            // Example:
            // f(x) = ...
            // f(1) = ... <- we're here
            // We have an adjoint for f(1) defined over the whole support of f
            // Now we want to initialize for the f(x) update
            // Need to propagate back to all x while masking 1
            // x -> next_args
            // 1 -> update_args
            if (update_id >= 0) {
                FuncKey next_func_key{ func.name(), update_id - 1 };
                Func &next_adjoint_func = adjoint_funcs[next_func_key];
                std::vector<Var> next_args = next_adjoint_func.args();
                std::vector<Expr> update_args = func.update_args(update_id);
                // Replace implicit variables
                for (auto &arg : update_args) {
                    std::set<std::string> implicit_variables =
                        find_implicit_variables(arg);
                    for (const auto &var : implicit_variables) {
                        arg = substitute(var, next_args[Var::implicit_index(var)], arg);
                    }
                }
                // Check if next_args are the same as update_args
                // If they are the same simply set everything to zero
                bool is_noop = true;
                for (int i = 0; i < (int) next_args.size(); i++) {
                    const Variable *update_var = update_args[i].as<Variable>();
                    if (update_var == nullptr || next_args[i].name() != update_var->name) {
                        is_noop = false;
                    }
                }
                next_adjoint_func = Func(next_adjoint_func.name());
                if (!is_noop) {
                    // f'(x) = adjoint
                    next_adjoint_func(next_args) =
                        adjoint_funcs[func_key](next_args);
                }
                if (func.values().size() == 1) {
                    Type type = func.values()[0].type();
                    next_adjoint_func(update_args) = make_const(type, 0.0);
                } else {
                    std::vector<Expr> init(func.values().size());
                    for (int i = 0; i < (int) init.size(); i++) {
                        init[i] = make_const(func.values()[i].type(), 0.0);
                    }
                    next_adjoint_func(update_args) = Tuple(init);
                }
            }

            // Now we want to propagate the derivatives at expression level
            // Topologically sort the expressions for each value in the tuple
            std::vector<Expr> expr_list;
            Tuple tuple = update_id < 0 ? func.values() : func.update_values(update_id);
            std::vector<const BaseExprNode *> output_exprs;
            auto tuple_vector = tuple.as_vector();
            for (const auto &expr : tuple_vector) {
                std::vector<Expr> value_expr_list = sort_expressions(expr);
                expr_list.insert(expr_list.end(), value_expr_list.begin(), value_expr_list.end());
                output_exprs.push_back((const BaseExprNode *) expr_list.back().get());
            }

            // TODO: replace let_var_mapping with Scope
            // Gather let variables
            let_var_mapping.clear();
            let_variables.clear();
            for (auto it = expr_list.begin(); it != expr_list.end(); it++) {
                Expr expr = *it;
                if (expr.get()->node_type == IRNodeType::Let) {
                    const Let *op = expr.as<Let>();
                    // Assume Let variables are unique
                    assert(let_var_mapping.find(op->name) == let_var_mapping.end());
                    let_var_mapping[op->name] = op->value;
                    let_variables.push_back(op->name);
                }
            }

            // Retrieve previously propagated adjoint for the Func,
            // apply it to expression adjoints
            std::vector<Expr> update_args;
            if (update_id >= 0) {
                update_args = func.update_args(update_id);
            } else {
                update_args.reserve(func.args().size());
                Func adjoint_func = adjoint_funcs[func_key];
                for (const auto &var : adjoint_func.args()) {
                    update_args.push_back(var);
                }
            }
            for (int i = 0; i < (int) output_exprs.size(); i++) {
                expr_adjoints[output_exprs[i]] =
                    Call::make(adjoint_funcs[func_key].function(), update_args, i);
            }

            // Traverse the expressions in reverse order
            for (auto it = expr_list.rbegin(); it != expr_list.rend(); it++) {
                // Propagate adjoints
                it->accept(this);
            }
            expr_adjoints.clear();
        }
    }
}

void ReverseAccumulationVisitor::accumulate(const Expr &stub, const Expr &adjoint) {
    const BaseExprNode *stub_ptr = (const BaseExprNode *) stub.get();
    if (expr_adjoints.find(stub_ptr) == expr_adjoints.end()) {
        expr_adjoints[stub_ptr] = adjoint;
    } else {
        expr_adjoints[stub_ptr] += adjoint;
    }
}

void ReverseAccumulationVisitor::visit(const Cast *op) {
    assert(expr_adjoints.find(op) != expr_adjoints.end());
    Expr adjoint = expr_adjoints[op];

    // d/dx cast(x) = 1.f if op->type is float otherwise 0
    if (op->type.is_float()) {
        accumulate(op->value, make_const(op->type, 1.0));
    } else {
        accumulate(op->value, make_const(op->type, 0));
    }
}

void ReverseAccumulationVisitor::visit(const Variable *op) {
    assert(expr_adjoints.find(op) != expr_adjoints.end());
    Expr adjoint = expr_adjoints[op];

    // If the variable is a let variable, accumulates adjoints into the content
    auto it = let_var_mapping.find(op->name);
    if (it != let_var_mapping.end()) {
        accumulate(it->second, Let::make(op->name, it->second, adjoint));
    }
}

void ReverseAccumulationVisitor::visit(const Add *op) {
    assert(expr_adjoints.find(op) != expr_adjoints.end());
    Expr adjoint = expr_adjoints[op];

    // d/da a + b = 1
    accumulate(op->a, adjoint);
    // d/db a + b = 1
    accumulate(op->b, adjoint);
}

void ReverseAccumulationVisitor::visit(const Sub *op) {
    assert(expr_adjoints.find(op) != expr_adjoints.end());
    Expr adjoint = expr_adjoints[op];

    // d/da a - b = 1
    accumulate(op->a, adjoint);
    // d/db a - b = -1
    accumulate(op->b, -adjoint);
}

void ReverseAccumulationVisitor::visit(const Mul *op) {
    assert(expr_adjoints.find(op) != expr_adjoints.end());
    Expr adjoint = expr_adjoints[op];

    // d/da a * b = b
    accumulate(op->a, adjoint * op->b);
    // d/db a * b = a
    accumulate(op->b, adjoint * op->a);
}

void ReverseAccumulationVisitor::visit(const Div *op) {
    assert(expr_adjoints.find(op) != expr_adjoints.end());
    Expr adjoint = expr_adjoints[op];

    // d/da a / b = 1 / b
    accumulate(op->a, adjoint / op->b);
    // d/db a / b = - a / b^2
    accumulate(op->b, -adjoint * op->a / (op->b * op->b));
}

void ReverseAccumulationVisitor::visit(const Min *op) {
    assert(expr_adjoints.find(op) != expr_adjoints.end());
    Expr adjoint = expr_adjoints[op];

    // d/da min(a, b) = a <= b ? 1 : 0
    accumulate(op->a,
               select(op->a <= op->b, adjoint, make_const(adjoint.type(), 0.0)));
    // d/db min(a, b) = b <= a ? 1 : 0
    accumulate(op->b,
               select(op->b <= op->a, adjoint, make_const(adjoint.type(), 0.0)));
}

void ReverseAccumulationVisitor::visit(const Max *op) {
    assert(expr_adjoints.find(op) != expr_adjoints.end());
    Expr adjoint = expr_adjoints[op];

    // d/da max(a, b) = a >= b ? 1 : 0
    accumulate(op->a,
               select(op->a >= op->b, adjoint, make_const(adjoint.type(), 0.0)));
    // d/db max(a, b) = b >= a ? 1 : 0
    accumulate(op->b,
               select(op->b >= op->a, adjoint, make_const(adjoint.type(), 0.0)));
}

void ReverseAccumulationVisitor::visit(const Let *op) {
    assert(expr_adjoints.find(op) != expr_adjoints.end());
    Expr adjoint = expr_adjoints[op];

    accumulate(op->body, adjoint);
}

void ReverseAccumulationVisitor::visit(const Select *op) {
    assert(expr_adjoints.find(op) != expr_adjoints.end());
    Expr adjoint = expr_adjoints[op];

    // d/db select(a, b, c) = select(a, 1, 0)
    accumulate(op->true_value,
               select(op->condition, adjoint, make_const(adjoint.type(), 0.0)));
    // d/dc select(a, b, c) = select(a, 0, 1)
    accumulate(op->false_value,
               select(op->condition, make_const(adjoint.type(), 0.0), adjoint));
}

void ReverseAccumulationVisitor::visit(const Call *op) {
    assert(expr_adjoints.find(op) != expr_adjoints.end());
    Expr adjoint = expr_adjoints[op];
    if (op->is_extern()) {
        // Math functions
        if (check_opname(op->name, "exp")) {
            // d/dx exp(x) = exp(x)
            accumulate(op->args[0], adjoint * exp(op->args[0]));
        } else if (check_opname(op->name, "log")) {
            // d/dx log(x) = 1 / x
            accumulate(op->args[0], adjoint / op->args[0]);
        } else if (check_opname(op->name, "sin")) {
            // d/dx sin(x) = cos(x)
            accumulate(op->args[0], adjoint * cos(op->args[0]));
        } else if (check_opname(op->name, "asin")) {
            // d/dx asin(x) = 1 / sqrt(1 - x^2)
            Expr one = make_const(op->type, 1.0);
            accumulate(op->args[0], adjoint / sqrt(one - op->args[0] * op->args[0]));
        } else if (check_opname(op->name, "cos")) {
            // d/dx cos(x) = -sin(x)
            accumulate(op->args[0], -adjoint * sin(op->args[0]));
        } else if (check_opname(op->name, "acos")) {
            // d/dx acos(x) = - 1 / sqrt(1 - x^2)
            Expr one = make_const(op->type, 1.0);
            accumulate(op->args[0], -adjoint / sqrt(one - op->args[0] * op->args[0]));
        } else if (check_opname(op->name, "tan")) {
            // d/dx tan(x) = 1 / cos(x)^2
            Expr c = cos(op->args[0]);
            accumulate(op->args[0], adjoint / (c * c));
        } else if (check_opname(op->name, "atan")) {
            // d/dx atan(x) = 1 / (1 + x^2)
            Expr one = make_const(op->type, 1.0);
            accumulate(op->args[0], adjoint / (one + op->args[0] * op->args[0]));
        } else if (check_opname(op->name, "atan2")) {
            Expr x2y2 = op->args[0] * op->args[0] + op->args[1] * op->args[1];
            // d/dy atan2(y, x) = x / (x^2 + y^2)
            accumulate(op->args[0], adjoint * op->args[1] / x2y2);
            // d/dx atan2(y, x) = -y / (x^2 + y^2)
            accumulate(op->args[1], -adjoint * op->args[0] / x2y2);
        } else if (check_opname(op->name, "sinh")) {
            // d/dx sinh(x) = cosh(x)
            accumulate(op->args[0], adjoint * cosh(op->args[0]));
        } else if (check_opname(op->name, "asinh")) {
            // d/dx asin(x) = 1 / sqrt(1 + x^2)
            Expr one = make_const(op->type, 1.0);
            accumulate(op->args[0], adjoint / sqrt(one + op->args[0] * op->args[0]));
        } else if (check_opname(op->name, "cosh")) {
            // d/dx cosh(x) = sinh(x)
            accumulate(op->args[0], adjoint * sinh(op->args[0]));
        } else if (check_opname(op->name, "acosh")) {
            // d/dx acosh(x) = 1 / (sqrt(x - 1) sqrt(x + 1)))
            Expr one = make_const(op->type, 1.0);
            accumulate(op->args[0],
                       adjoint / (sqrt(op->args[0] - one) * sqrt(op->args[0] + one)));
        } else if (check_opname(op->name, "tanh")) {
            // d/dx tanh(x) = 1 / cosh(x)^2
            Expr c = cosh(op->args[0]);
            accumulate(op->args[0], adjoint / (c * c));
        } else if (check_opname(op->name, "atanh")) {
            // d/dx atanh(x) = 1 / (1 - x^2)
            Expr one = make_const(op->type, 1.0);
            accumulate(op->args[0], adjoint / (one - op->args[0] * op->args[0]));
        } else if (check_opname(op->name, "ceil")) {
            // TODO: d/dx = dirac(n) for n in Z ...
            accumulate(op->args[0], make_const(op->type, 0.0));
        } else if (check_opname(op->name, "floor")) {
            // TODO: d/dx = dirac(n) for n in Z ...
            accumulate(op->args[0], make_const(op->type, 0.0));
        } else if (check_opname(op->name, "round")) {
            accumulate(op->args[0], make_const(op->type, 0.0));
        } else if (check_opname(op->name, "trunc")) {
            accumulate(op->args[0], make_const(op->type, 0.0));
        } else if (check_opname(op->name, "sqrt")) {
            Expr half = make_const(op->type, 0.5);
            accumulate(op->args[0], adjoint * half / sqrt(op->args[0]));
        } else if (check_opname(op->name, "pow")) {
            Expr one = make_const(op->type, 1.0);
            accumulate(op->args[0],
                       adjoint * op->args[1] * pow(op->args[0], op->args[1] - one));
            accumulate(op->args[1],
                       adjoint * pow(op->args[0], op->args[1]) * log(op->args[0]));
        } else if (check_opname(op->name, "fast_inverse")) {
            // d/dx 1/x = -1/x^2
            Expr inv_x = fast_inverse(op->args[0]);
            accumulate(op->args[0], -adjoint * inv_x * inv_x);
        } else if (check_opname(op->name, "fast_inverse_sqrt")) {
            // d/dx x^(-0.5) = -0.5*x^(-1.5)
            Expr inv_sqrt_x = fast_inverse_sqrt(op->args[0]);
            Expr neg_half = make_const(op->type, -0.5);
            accumulate(op->args[0],
                       neg_half * adjoint * inv_sqrt_x * inv_sqrt_x * inv_sqrt_x);
        } else if (op->name == "halide_print") {
            accumulate(op->args[0], make_const(op->type, 0.0));
        } else {
            internal_error << "The derivative of " << op->name << " is not implemented.";
        }
    } else if (op->is_intrinsic()) {
        if (op->is_intrinsic(Call::abs)) {
            accumulate(op->args[0],
                       adjoint * select(op->args[0] > 0,
                                        make_const(op->type, 1.0), make_const(op->type, -1.0)));
        } else if (op->is_intrinsic(Call::lerp)) {
            // z = x * (1 - w) + y * w
            // dz/dx = 1 - w
            // dz/dy = w
            // dz/dw = y - x
            accumulate(op->args[0], adjoint * (make_const(op->type, 1.0) - op->args[2]));
            accumulate(op->args[1], adjoint * op->args[2]);
            accumulate(op->args[2], adjoint * (op->args[1] - op->args[0]));
        } else if (op->is_intrinsic(Call::likely)) {
            accumulate(op->args[0], adjoint);
        } else if (op->is_intrinsic(Call::return_second)) {
            accumulate(op->args[0], make_const(op->type, 0.0));
            accumulate(op->args[1], adjoint);
        } else if (op->is_intrinsic(Call::undef)) {
            // do nothing
        } else {
            user_warning << "Dropping gradients at call to " << op->name << "\n";
            for (const auto &arg : op->args) {
                accumulate(arg, make_const(op->type, 0.0));
            }
        }
    } else if (op->call_type == Call::Halide ||
               op->call_type == Call::Image) {  // Halide function call or Halid buffer
        // TODO: check if we need this elsewhere
        // Add Let expressions
        adjoint = add_let_expression(adjoint, let_var_mapping, let_variables);
        std::vector<Expr> lhs = op->args;
        for (int i = 0; i < (int) lhs.size(); i++) {
            lhs[i] = add_let_expression(lhs[i], let_var_mapping, let_variables);
        }
        Expr adjoint_before_canonicalize = adjoint;
        std::vector<Expr> lhs_before_canonicalize = lhs;

        // If target is the current function itself, send to previous update
        // e.g. f(x) = ...
        //      f(x) = f(x) + 1
        // We create a function for the initial condition and each update
        // When update i uses value from update i-1, we accumulate the
        // adjoints to update i-1
        FuncKey func_key;
        if (op->func.defined()) {
            Function func(op->func);
            func_key = func.name() != current_func.name() ? FuncKey{ func.name(), func.updates().size() - 1 } : FuncKey{ func.name(), current_update_id - 1 };
        } else {
            func_key = FuncKey{ op->name, -1 };
        }
        assert(adjoint_funcs.find(func_key) != adjoint_funcs.end());
        Func &func_to_update = adjoint_funcs[func_key];
        assert(func_to_update.dimensions() == (int) lhs.size());

        bool debug_flag = false;

        if (debug_flag) {
            debug(0) << "current_func:" << current_func.name() << "\n";
            debug(0) << "Scattering to " << op->name << "\n";
            debug(0) << "lhs is:";
            for (const auto &arg : lhs) {
                debug(0) << " " << arg;
            }
            debug(0) << "\n";
            debug(0) << "adjoint is:" << simplify(adjoint) << "\n";
        }

        // Gather argument & bounds information
        // current_args are the pure variables
        // current_update_args are the actual updates at left hand side
        Func current_adjoint_func =
            adjoint_funcs[FuncKey{ current_func.name(), current_update_id }];
        std::vector<Var> current_args = current_adjoint_func.args();
        std::vector<Expr> current_update_args;
        if (current_update_id >= 0) {
            current_update_args = current_func.update_args(current_update_id);
        } else {
            current_update_args.reserve(current_args.size());
            for (const auto &var : current_args) {
                current_update_args.push_back(var);
            }
        }
        const Box &current_bounds = func_bounds[current_func.name()];

        // Replace implicit variables
        for (auto &arg : lhs) {
            std::set<std::string> implicit_variables = find_implicit_variables(arg);
            for (const auto &var : implicit_variables) {
                arg = substitute(var, current_args[Var::implicit_index(var)], arg);
            }
        }
        {
            std::set<std::string> implicit_variables =
                find_implicit_variables(adjoint);
            for (const auto &var : implicit_variables) {
                adjoint = substitute(
                    var, current_args[Var::implicit_index(var)], adjoint);
            }
        }

        // We want to do this:
        // func_to_update(op->args) += adjoint(current_update_args);
        // But op->args can be invalid lhs, need to canonicalize.
        // We canonicalize by first trying to substitute with pure variables.
        // If that fails we will replace variables on lhs with RDoms
        // (general scattering).

        // We try canonicalize the left hand side arguments (op->args)
        // so that it's always x, y, z, ...
        //
        // Given:
        // g(x, y, z) = f(x, y-1, z+1)
        // we get an invalid update:
        // f'(x, y - 1, z + 1) += g'(x, y, z)
        // Goal: rewrite to
        //  ==> f'(x, y, z) += g'(x, y+1, z-1)
        // (below we would call g and g' the "current function" and
        //  we call f and d_f the "function to update")
        //
        // We do this by set up a new set of variables new_args
        // new_args contains a set of variable u0, u1, u2, ...
        // For each left hand side of the update (x, y - 1, z + 1 here),
        // we set up the equation u0 = x, u1 = y - 1, u2 = z + 1.
        // Then we solve for x, y, z and get x = u0, y = u1 + 1, z = u2 - 1
        // We would get f'(u0, u1, u2) += g'(u0, u1 + 1, u2 - 1)
        // We then substitute the original variable names back to get
        // f'(x, y, z) += g'(x, x + 1, z - 1)
        //
        // Currently we don't want to mess with system solving yet,
        // So we gather all arguments that contains multiple pure variables,
        // and invalidate all of them.
        // Inter-dependencies like:
        // g(x, y) = f(x * y, x + y)
        // can't be simplified.
        // In principle this can be inverted by solving a system of equations.
        // In this case we replace x and y with reduction variables that loop
        // through g's bounds
        // i.e.
        // f'(r.x * r.y, r.x + r.y) += g'(r.x, r.y)

        // Prepare a set of new substitution variables for func_to_update
        std::vector<Var> new_args;
        new_args.reserve(func_to_update.args().size());
        for (int arg_id = 0; arg_id < (int) func_to_update.args().size(); arg_id++) {
            new_args.push_back(Var("u" + std::to_string(arg_id) + "_"));
        }

        // Loop over the left hand side of the update, construct equations
        // and invert them.
        std::vector<bool> canonicalized(lhs.size(), false);
        std::set<std::string> canonicalized_vars;
        for (int arg_id = 0; arg_id < (int) lhs.size(); arg_id++) {
            // Gather all pure variables at op->args[arg_id],
            // substitute them with new_args
            // For now only support single pure variable
            std::vector<std::string> variables =
                gather_variables(lhs[arg_id], vars_to_strings(current_args));
            if (variables.size() != 1) {
                continue;
            }

            bool solved;
            Expr result_rhs;
            std::tie(solved, result_rhs) =
                solve_inverse(new_args[arg_id] == lhs[arg_id],
                              new_args[arg_id].name(),
                              variables[0]);
            if (!solved) {
                continue;
            }

            // Replace pure variable with the reverse.
            // Make sure to also substitute predicates
            adjoint = substitute_rdom_predicate(variables[0], result_rhs, adjoint);

            // Since we successfully invert, the left hand side becomes
            // new_args
            lhs[arg_id] = new_args[arg_id];
            // Record that we sucessfully invert, for those we fail
            // we need to perform general scattering.
            canonicalized[arg_id] = true;
            canonicalized_vars.insert(variables[0]);
        }

        // Sometimes the canonicalization above fails.
        // We replace the pure variables inside lhs with RDoms for general scattering
        std::vector<std::pair<Expr, Expr>> bounds;
        bounds.reserve(current_args.size());
        for (int arg_id = 0; arg_id < (int) current_args.size(); arg_id++) {
            bounds.push_back({ current_bounds[arg_id].min,
                               current_bounds[arg_id].max - current_bounds[arg_id].min + 1 });
        }
        RDom r_bounds(bounds);
        for (int lhs_id = 0; lhs_id < (int) lhs.size(); lhs_id++) {
            if (!canonicalized[lhs_id]) {
                Expr lhs_arg = lhs[lhs_id];
                std::vector<std::string> variables =
                    gather_variables(lhs_arg, current_adjoint_func.function().args());
                RDom r(bounds);
                // For each variable found in lhs_arg, find the corresponding
                // bound (by looping through all variables) and substitute
                // with the bound reduction variable.
                for (int var_id = 0; var_id < (int) variables.size(); var_id++) {
                    for (int arg_id = 0; arg_id < (int) current_args.size(); arg_id++) {
                        if (current_args[arg_id].name() == variables[var_id] &&
                            canonicalized_vars.find(
                                current_args[arg_id].name()) ==
                                canonicalized_vars.end()) {
                            lhs[lhs_id] = substitute(variables[var_id],
                                                     r_bounds[arg_id],
                                                     lhs[lhs_id]);
                            adjoint = substitute(
                                variables[var_id], r_bounds[arg_id], adjoint);
                            break;
                        }
                    }
                }
            }
        }

        // For each free variable on the rhs, replace it with current bounds
        // e.g. we have in forward pass f(x, y) = g(x)
        //      then we would have g'(x) += f'(x, y) by now
        //      now we need to replace y with a reduction variable over f's bound
        //      x is automatically excluded since it's currently
        //      replaced by the new substitution variable e.g. u_0

        // First gather all free variables
        FuncBounds bounds_subset;
        std::vector<int> arg_id_to_substitute;
        bounds_subset.reserve(current_args.size());
        arg_id_to_substitute.reserve(current_args.size());
        for (int arg_id = 0; arg_id < (int) current_args.size(); arg_id++) {
            if (has_variable(adjoint, current_args[arg_id].name())) {
                const Interval &interval = current_bounds[arg_id];
                bounds_subset.emplace_back(
                    interval.min, interval.max - interval.min + 1);
                arg_id_to_substitute.push_back(arg_id);
            }
        }

        // Create a new RDom to loop over all free variables
        if (arg_id_to_substitute.size() > 0) {
            RDom r(bounds_subset);
            for (int i = 0; i < (int) arg_id_to_substitute.size(); i++) {
                int arg_id = arg_id_to_substitute[i];
                adjoint = substitute(current_args[arg_id].name(), r[i], adjoint);
            }
        }

        // General scattering simplification rules
        // For each expression in lhs,
        // check if it is an expression of a single rvar and
        // spans the same interval of the function's bound
        // if so we can rewrite it back to pure variables
        // e.g.
        // f(r.x) = g(r.x)
        // => f(x) = g(x)
        // Another common pattern is the reverse of downsampling
        // if we see s * r.x + r.y and r.y has min == 0 and extent == s
        // we simplify them to x and replace all occurence of r.x by x/4
        // e.g.
        // f(4 * r.x + r.y) = g(r.x) + h(4 * r.x + r.y)
        // => f(x) = g(x/4) + h(x)
        std::vector<Var> func_to_update_args = func_to_update.args();
        for (int i = 0; i < (int) lhs.size(); i++) {
            Expr lhs_arg = substitute_in_all_lets(lhs[i]);
            const Variable *var = lhs_arg.as<Variable>();
            const Add *add = lhs_arg.as<Add>();
            // f(r.x) = g(r.x)
            // => f(x) = g(x)
            if (var != nullptr && var->reduction_domain.defined() &&
                var->reduction_domain.split_predicate().size() == 0) {
                ReductionDomain rdom = var->reduction_domain;
                int rvar_id = -1;
                for (int rid = 0; rid < (int) rdom.domain().size(); rid++) {
                    if (rdom.domain()[rid].var == var->name) {
                        rvar_id = rid;
                        break;
                    }
                }
                assert(rvar_id != -1);
                ReductionVariable rvar = rdom.domain()[rvar_id];
                // Check if the min/max of the rvariable is the same as the target function
                const Box &target_bounds = func_bounds[op->name];
                Interval t_interval = target_bounds[i];
                t_interval.min = simplify(t_interval.min);
                t_interval.max = simplify(t_interval.max);
                Interval r_interval(simplify(rvar.min),
                                    simplify(rvar.min + rvar.extent - 1));
                if (can_prove(r_interval.min <= t_interval.min &&
                              r_interval.max >= t_interval.max)) {
                    lhs[i] = func_to_update_args[i];
                    // Replace other occurence of rvar in lhs
                    for (int j = 0; j < (int) lhs.size(); j++) {
                        if (j != i) {
                            lhs[j] = simplify(substitute(
                                rvar.var, func_to_update_args[i], lhs[j]));
                        }
                    }
                    adjoint = simplify(substitute(
                        rvar.var, func_to_update_args[i], adjoint));
                }
                // f(4 * r.x + r.y) = g(r.x) + h(4 * r.x + r.y)
                // => f(x) = g(x/4) + h(x)
            } else if (add != nullptr &&
                       ((add->a.as<Mul>() != nullptr &&
                         add->b.as<Variable>() != nullptr) ||
                        (add->a.as<Variable>() != nullptr &&
                         add->b.as<Mul>() != nullptr))) {
                // Find pattern s * r.x + r.y where r.y.min == 0 && r.y.extent == s
                Expr a = add->a, b = add->b;
                if (add->b.as<Mul>() != nullptr) {
                    // swap so that b is always the Variable
                    assert(add->a.as<Variable>() != nullptr);
                    std::swap(a, b);
                }
                const Mul *mul = a.as<Mul>();
                const Variable *b_var = b.as<Variable>();
                assert(mul != nullptr && b_var != nullptr);
                Expr mul_a = mul->a, mul_b = mul->b;
                if (mul_a.as<Variable>() != nullptr &&
                    mul_a.as<Variable>()->reduction_domain.defined()) {
                    std::swap(mul_a, mul_b);
                }
                const Variable *mul_b_var = mul_b.as<Variable>();
                if (mul_b_var == nullptr || !mul_b_var->reduction_domain.defined()) {
                    continue;
                }
                ReductionDomain b_rdom = b_var->reduction_domain;
                if (!b_rdom.defined()) {
                    continue;
                }

                int rvar_id = -1;
                for (int rid = 0; rid < (int) b_rdom.domain().size(); rid++) {
                    if (b_rdom.domain()[rid].var == b_var->name) {
                        rvar_id = rid;
                        break;
                    }
                }
                assert(rvar_id != -1);
                ReductionVariable rvar = b_rdom.domain()[rvar_id];
                if (!equal(rvar.min, Expr(0)) || !equal(rvar.extent, mul_a)) {
                    continue;
                }

                // We've finally made sure that the expression has the form we want
                // Now replace everything
                // replace s * r.x + r.y with x
                lhs[i] = func_to_update_args[i];
                adjoint = substitute(lhs_arg,
                                     func_to_update_args[i],
                                     substitute_in_all_lets(adjoint));
                // replace r.x with x / s
                adjoint = substitute(mul_b, func_to_update_args[i] / mul_a, adjoint);
                adjoint = simplify(adjoint);
            }
        }

        // We can only have one RDom for each update.
        // Therefore we have to merge RDoms on both lhs and rhs
        // To make use of better locality we preserve partial order
        std::map<std::string, ReductionVariableInfo> rvar_maps =
            gather_rvariables(adjoint);
        for (const auto &lhs_arg : lhs) {
            std::map<std::string, ReductionVariableInfo> maps =
                gather_rvariables(lhs_arg);
            rvar_maps.insert(maps.begin(), maps.end());
        }
        // Original set of reduction variables
        std::map<std::string, ReductionVariableInfo> org_rvar_maps =
            gather_rvariables(adjoint_before_canonicalize);
        for (const auto &lhs_arg : lhs_before_canonicalize) {
            std::map<std::string, ReductionVariableInfo> maps =
                gather_rvariables(lhs_arg);
            org_rvar_maps.insert(maps.begin(), maps.end());
        }
        // Order: newly introduced rvar -> original rvar
        std::vector<ReductionVariableInfo> new_rvar_vec, old_rvar_vec;
        for (const auto &it : rvar_maps) {
            if (org_rvar_maps.find(it.first) == org_rvar_maps.end()) {
                new_rvar_vec.push_back(it.second);
            } else {
                old_rvar_vec.push_back(it.second);
            }
        }
        // Sort by index & domain
        auto cmp_rv = [](const ReductionVariableInfo &rv0,
                         const ReductionVariableInfo &rv1) {
            ReductionDomain::Compare cmp;
            if (cmp(rv0.domain, rv1.domain)) {
                return true;
            } else {
                return rv0.index < rv1.index;
            }
        };
        std::sort(new_rvar_vec.begin(), new_rvar_vec.end(), cmp_rv);
        std::sort(old_rvar_vec.begin(), old_rvar_vec.end(), cmp_rv);
        // Flatten to an array
        std::vector<std::string> var_names;
        FuncBounds merged_bounds;
        for (const auto &it : new_rvar_vec) {
            var_names.push_back(it.name);
            merged_bounds.emplace_back(it.min, it.extent);
        }
        for (const auto &it : old_rvar_vec) {
            var_names.push_back(it.name);
            merged_bounds.emplace_back(it.min, it.extent);
        }
        // Produce final merged RDom
        RDom merged_r;
        if (merged_bounds.size() > 0) {
            merged_r = RDom(merged_bounds);
            // Transfer the predicate from old RDoms to merged RDom
            // Gather the set of RDoms
            std::set<ReductionDomain, ReductionDomain::Compare> rdoms;
            for (const auto &it : rvar_maps) {
                rdoms.insert(it.second.domain);
            }
            Expr rdom_predicate = Internal::UIntImm::make(UInt(1), 1);
            for (const auto &rdom : rdoms) {
                rdom_predicate = simplify(rdom_predicate && rdom.predicate());
            }
            // Reference to new RDom
            for (int rid = 0; rid < merged_r.dimensions(); rid++) {
                adjoint = substitute(var_names[rid], merged_r[rid], adjoint);
                for (auto &lhs_arg : lhs) {
                    lhs_arg = substitute(var_names[rid], merged_r[rid], lhs_arg);
                }
                rdom_predicate = substitute(
                    var_names[rid], merged_r[rid], rdom_predicate);
            }
            if (!is_const(rdom_predicate)) {
                for (int arg_id = 0; arg_id <
                                     (int) func_to_update_args.size();
                     arg_id++) {
                    // Substitute new_args back to original variables
                    rdom_predicate = substitute(new_args[arg_id].name(),
                                                func_to_update_args[arg_id], rdom_predicate);
                }
                merged_r.where(rdom_predicate);
            }
        }

        // Substitute new_args back to original variables
        for (int arg_id = 0; arg_id < (int) func_to_update_args.size(); arg_id++) {
            for (auto &lhs_arg : lhs) {
                lhs_arg = substitute(new_args[arg_id].name(),
                                     func_to_update_args[arg_id], lhs_arg);
            }
            adjoint = substitute_rdom_predicate(
                new_args[arg_id].name(), func_to_update_args[arg_id], adjoint);
        }
        adjoint = simplify(adjoint);

        // Finally we update the function definitions,
        // possibly merge with previous updates
        auto can_merge = [&]() -> bool {
            if (func_to_update.num_update_definitions() == 0) {
                // If lhs are not pure variables we can't merge to pure definition
                for (int i = 0; i < (int) lhs.size(); i++) {
                    if (!equal(lhs[i], func_to_update.args()[i])) {
                        return false;
                    }
                }
                ReductionDomain rdom = extract_rdom(adjoint);
                // If there are rdoms in adjoint we can't merge
                return !rdom.defined();
            }
            int update_id = func_to_update.num_update_definitions() - 1;
            std::vector<Expr> prev_lhs =
                func_to_update.update_args(update_id);
            assert(prev_lhs.size() == lhs.size());
            // If previous update has different left hand side, don't merge
            for (int i = 0; i < (int) prev_lhs.size(); i++) {
                if (!equal(lhs[i], prev_lhs[i])) {
                    return false;
                }
            }
            // If previous update has a different set of reduction variables,
            // don't merge
            const std::vector<ReductionVariable> &rvars =
                func_to_update.update(update_id).get_schedule().rvars();
            if (!merged_r.defined()) {
                return rvars.size() == 0;
            }
            if ((int) rvars.size() != merged_r.dimensions()) {
                return false;
            }

            for (int i = 0; i < (int) rvars.size(); i++) {
                if (!equal(rvars[i].min, merged_r[i].min())) {
                    return false;
                }
                if (!equal(rvars[i].extent, merged_r[i].extent())) {
                    return false;
                }
            }
            return true;
        };

        if (debug_flag) {
            debug(0) << "func_to_update.name():" << func_to_update.name() << "\n";
            debug(0) << "lhs after canonicalization:";
            for (const auto &arg : lhs) {
                debug(0) << " " << arg;
            }
            debug(0) << "\n";
            debug(0) << "adjoint after canonicalization:" << simplify(adjoint) << "\n";
        }

        // TODO: maybe do some analysis on lhs to avoid applying boundary conditions to
        //       function calls in adjoint
        if (!can_merge()) {
            if (func_to_update.values().size() == 1) {
                func_to_update(lhs) += adjoint;
            } else {
                func_to_update(lhs)[op->value_index] += adjoint;
            }
        } else {
            Definition &def = func_to_update.num_update_definitions() == 0 ? func_to_update.function().definition() : func_to_update.function().update(func_to_update.num_update_definitions() - 1);
            std::vector<Expr> &values = def.values();
            ReductionDomain rdom;
            for (const auto &val : values) {
                rdom = extract_rdom(val);
                if (rdom.defined()) {
                    break;
                }
            }
            if (rdom.defined()) {
                assert(func_to_update.num_update_definitions() > 0);
                // Make sure we're using the same set of reduction variables
                for (int i = 0; i < merged_r.dimensions(); i++) {
                    adjoint = substitute(merged_r[i].name(), RVar(rdom, i), adjoint);
                }
            }

            if (values.size() == 1) {
                values[0] = simplify(values[0] + adjoint);
            } else {
                const Add *add = values[op->value_index].as<Add>();
                if (add != nullptr &&
                    add->b.as<Call>() != nullptr &&
                    add->b.as<Call>()->is_intrinsic(Call::undef)) {
                    // Sometimes the expression is an undef for the case of a tuple.
                    // Make sure we don't include the undefs
                    values[op->value_index] = simplify(add->a + adjoint);
                } else {
                    values[op->value_index] =
                        simplify(values[op->value_index] + adjoint);
                }
            }
        }
    } else {
        // TODO: let user provide derivatives for external functions
        internal_error << "Unknown call type of operation: " << op->name << "\n";
    }
}

Expr forward_accumulation(const Expr &expr,
                          const std::map<std::string, Func> &tangents,
                          Scope<Expr> &scope) {
    if (const Cast *op = expr.as<Cast>()) {
        Expr t = forward_accumulation(op->value, tangents, scope);
        return Cast::make(op->type, t);
    } else if (const Add *op = expr.as<Add>()) {
        // d/dx f(x) + g(x) = d/dx f(x) + d/dx g(x)
        Expr a = forward_accumulation(op->a, tangents, scope);
        Expr b = forward_accumulation(op->b, tangents, scope);
        return a + b;
    } else if (const Sub *op = expr.as<Sub>()) {
        // d/dx f(x) - g(x) = d/dx f(x) - d/dx g(x)
        Expr a = forward_accumulation(op->a, tangents, scope);
        Expr b = forward_accumulation(op->b, tangents, scope);
        return a - b;
    } else if (const Mul *op = expr.as<Mul>()) {
        // d/dx f(x) g(x) = g(x) d/dx f(x) + f(x) d/dx g(x)
        Expr a = forward_accumulation(op->a, tangents, scope);
        Expr b = forward_accumulation(op->b, tangents, scope);
        return simplify(op->a * b + a * op->b);
    } else if (const Div *op = expr.as<Div>()) {
        // d/dx f(x) / g(x) = (f'g - g'f) / g^2
        Expr a = forward_accumulation(op->a, tangents, scope);
        Expr b = forward_accumulation(op->b, tangents, scope);
        return simplify(((op->b * a - op->a * b) / (op->b * op->b)));
    } else if (const Min *op = expr.as<Min>()) {
        Expr a = forward_accumulation(op->a, tangents, scope);
        Expr b = forward_accumulation(op->b, tangents, scope);
        return simplify(select(op->a < op->b, a, b));
    } else if (const Max *op = expr.as<Max>()) {
        Expr a = forward_accumulation(op->a, tangents, scope);
        Expr b = forward_accumulation(op->b, tangents, scope);
        return simplify(select(op->a > op->b, a, b));
    } else if (const Select *op = expr.as<Select>()) {
        Expr true_value = forward_accumulation(op->true_value, tangents, scope);
        Expr false_value = forward_accumulation(op->false_value, tangents, scope);
        return select(op->condition, true_value, false_value);
    } else if (const Let *op = expr.as<Let>()) {
        Expr value = forward_accumulation(op->value, tangents, scope);
        std::string fwd_name = op->name + ".fwd";
        scope.push(op->name, Variable::make(op->type, fwd_name));
        Expr body = forward_accumulation(op->body, tangents, scope);
        scope.pop(op->name);
        return Let::make(op->name, op->value,
                         Let::make(fwd_name, value, body));
    } else if (const Variable *op = expr.as<Variable>()) {
        if (scope.contains(op->name)) {
            return scope.get(op->name);
        } else {
            return make_const(op->type, 0.0);
        }
    } else if (const Call *op = expr.as<Call>()) {
        if (op->is_extern()) {
            if (check_opname(op->name, "exp")) {
                // d/dx exp(f(x)) = exp(f(x)) f'
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                return expr * d;
            } else if (check_opname(op->name, "log")) {
                // d/dx log(f(x)) = f' / f(x)
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                return d / expr;
            } else if (check_opname(op->name, "sin")) {
                // d/dx sin(f(x)) = cos(f(x)) f'
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                return cos(op->args[0]) * d;
            } else if (check_opname(op->name, "asin")) {
                // d/dx asin(f(x)) = f' / sqrt(1 - f(x)^2)
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                Expr one = make_const(op->type, 1.0);
                return d / sqrt(one - op->args[0] * op->args[0]);
            } else if (check_opname(op->name, "cos")) {
                // d/dx cos(f(x)) = -sin(f(x)) f'
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                return -sin(op->args[0]) * d;
            } else if (check_opname(op->name, "acos")) {
                // d/dx acos(f(x)) = -f' / sqrt(1 - f(x)^2)
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                Expr one = make_const(op->type, 1.0);
                return -d / sqrt(one - op->args[0] * op->args[0]);
            } else if (check_opname(op->name, "tan")) {
                // d/dx tan(f(x)) = f' / cos^2(f(x))
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                Expr cos_x = cos(op->args[0]);
                return d / (cos_x * cos_x);
            } else if (check_opname(op->name, "atan")) {
                // d/dx tan(f(x)) = f' / cos^2(f(x))
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                Expr one = make_const(op->type, 1.0);
                return d / (op->args[0] * op->args[0] + one);
            } else if (check_opname(op->name, "atan2")) {
                // d/dx atan2(f(x), g(x)) =
                //   f' * (g(x) / (f(x)^2 + g(x)^2)) -
                //   g' * (f(x) / (f(x)^2 + g(x)^2))
                Expr d0 = forward_accumulation(op->args[0], tangents, scope);
                Expr d1 = forward_accumulation(op->args[1], tangents, scope);
                Expr norm = op->args[0] * op->args[0] + op->args[1] * op->args[1];
                return (d0 * op->args[1] - d1 * op->args[0]) / norm;
            } else if (check_opname(op->name, "sinh")) {
                // d/dx sinh(f(x)) = f'cosh(f(x))
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                return d * cosh(op->args[0]);
            } else if (check_opname(op->name, "asinh")) {
                // d/dx asinh(f(x)) = f' / sqrt(f(x)^2 + 1)
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                Expr one = make_const(op->type, 1.0);
                return d / sqrt(one + op->args[0] * op->args[0]);
            } else if (check_opname(op->name, "cosh")) {
                // d/dx cosh(f(x)) = f'sinh(f(x))
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                return d * sinh(op->args[0]);
            } else if (check_opname(op->name, "acosh")) {
                // d/dx asinh(f(x)) = f' / sqrt((f(x) - 1) * (f(x) + 1))
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                Expr one = make_const(op->type, 1.0);
                return d / sqrt((op->args[0] - one) * (op->args[0] + one));
            } else if (check_opname(op->name, "tanh")) {
                // d/dx sinh(f(x)) = f'/cosh(f(x))^2
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                Expr cosh_x = cosh(op->args[0]);
                return d / (cosh_x * cosh_x);
            } else if (check_opname(op->name, "atanh")) {
                // d/dx sinh(f(x)) = f'/(1 - f(x)^2)
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                Expr one = make_const(op->type, 1.0);
                return d / (one - op->args[0] * op->args[0]);
            } else if (check_opname(op->name, "ceil")) {
                return make_const(op->type, 0.0);
            } else if (check_opname(op->name, "floor")) {
                return make_const(op->type, 0.0);
            } else if (check_opname(op->name, "round")) {
                return make_const(op->type, 0.0);
            } else if (check_opname(op->name, "trunc")) {
                return make_const(op->type, 0.0);
            } else if (check_opname(op->name, "sqrt")) {
                // d/dx f(x)^(0.5) = 0.5 * f(x)^(-0.5) f'
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                return (0.5f * d / expr);
            } else if (check_opname(op->name, "pow")) {
                // d/dx pow(f(x), g(x)) = pow(f(x), g(x)-1) *
                //                        (g(x) f'(x) + f(x) log(f(x))g'(x))
                Expr a = forward_accumulation(op->args[0], tangents, scope);
                Expr b = forward_accumulation(op->args[1], tangents, scope);
                return pow(op->args[0], op->args[1] - 1.f) *
                       (op->args[1] * a +
                        // Special hack: if g' == 0 then even if f == 0 the following term is 0
                        // basically we want -Inf * 0 = 0
                        select(b == 0.f,
                               make_const(op->type, 0.0),
                               op->args[0] * log(op->args[0]) * b));
            } else if (check_opname(op->name, "fast_inverse")) {
                // d/dx f(x)^(-1) = -f' * f(x)^(-2)
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                Expr inv_x = fast_inverse(op->args[0]);
                return -d * inv_x * inv_x;
            } else if (check_opname(op->name, "fast_inverse_sqrt")) {
                // d/dx f(x)^(-0.5) = -0.5 * f' * f(x)^(-1.5)
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                Expr inv_sqrt_x = fast_inverse_sqrt(op->args[0]);
                Expr neg_half = make_const(op->type, -0.5);
                return neg_half * d * inv_sqrt_x * inv_sqrt_x * inv_sqrt_x;
            } else if (op->name == "halide_print") {
                return make_const(op->type, 0.0);
            } else {
                internal_error << "The derivative of " << op->name << " is not implemented.";
            }
        } else if (op->call_type == Call::Image || op->call_type == Call::Halide) {
            auto it = tangents.find(op->name);
            if (it != tangents.end()) {
                Func tangent = it->second;
                return tangent(op->args);
            } else {
                return make_const(op->type, 0.0);
            }
        } else {
            internal_assert(op->is_intrinsic());
            if (op->is_intrinsic(Call::abs)) {
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                return select(op->args[0] > 0, d, -d);
            } else if (op->is_intrinsic(Call::lerp)) {
                // z = a(x) * (1 - w(x)) + b(x) * w(x)
                // dz/dx = -(w - 1) a' + (b - a) w' + w b'
                Expr a = forward_accumulation(op->args[0], tangents, scope);
                Expr b = forward_accumulation(op->args[1], tangents, scope);
                Expr w = forward_accumulation(op->args[2], tangents, scope);
                return -(op->args[2] - 1.f) * a + (op->args[1] - op->args[0]) * w + op->args[2] * b;
            } else if (op->is_intrinsic(Call::likely)) {
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                return likely(d);
            } else if (op->is_intrinsic(Call::return_second)) {
                Expr d = forward_accumulation(op->args[1], tangents, scope);
                return d;
            } else if (op->is_intrinsic(Call::stringify)) {
                return make_const(op->type, 0.0);
            } else if (op->is_intrinsic(Call::undef)) {
                return make_const(op->type, 0.0);
            } else if (op->is_intrinsic(Call::reinterpret)) {
                Expr d = forward_accumulation(op->args[0], tangents, scope);
                if (is_zero(d)) {
                    return d;
                } else {
                    internal_error << "Can't take a derivative through a reinterpret_cast\n";
                }
            } else {
                internal_error << "The derivative of intrinsic " << op->name << " is not implemented in call: " << Expr(op) << "\n";
            }
        }
    } else {
        return make_const(expr.type(), 0.0);
    }
    return make_const(expr.type(), 0.0);
}

Expr forward_accumulation(const Expr &expr,
                          const std::map<std::string, Func> &tangents) {
    Scope<Expr> scope;
    return forward_accumulation(expr, tangents, scope);
}

}  // namespace Internal

Derivative propagate_adjoints(const Func &output,
                              const Func &adjoint,
                              const std::vector<std::pair<Expr, Expr>> &output_bounds) {
    user_assert(output.dimensions() == adjoint.dimensions())
        << "output dimensions and adjoint dimensions must match\n";
    user_assert((int) output_bounds.size() == adjoint.dimensions())
        << "output_bounds and adjoint dimensions must match\n";

    Internal::ReverseAccumulationVisitor visitor;
    visitor.propagate_adjoints(output, adjoint, output_bounds);
    return Derivative{ visitor.get_adjoint_funcs() };
}

Derivative propagate_adjoints(const Func &output,
                              const Buffer<float> &adjoint) {
    user_assert(output.dimensions() == adjoint.dimensions());
    std::vector<std::pair<Expr, Expr>> bounds;
    for (int dim = 0; dim < adjoint.dimensions(); dim++) {
        bounds.push_back(std::make_pair(Expr(adjoint.min(dim)),
                                        Expr(adjoint.min(dim) + adjoint.extent(dim) - 1)));
    }
    Func adjoint_func("adjoint_func");
    adjoint_func(_) = adjoint(_);
    return propagate_adjoints(output, adjoint_func, bounds);
}

Derivative propagate_adjoints(const Func &output) {
    Func adjoint("adjoint");
    adjoint(output.args()) = Internal::make_const(output.value().type(), 1.0);
    std::vector<std::pair<Expr, Expr>> output_bounds;
    output_bounds.reserve(output.dimensions());
    for (int i = 0; i < output.dimensions(); i++) {
        output_bounds.push_back({ 0, 0 });
    }
    return propagate_adjoints(output, adjoint, output_bounds);
}

Func propagate_tangents(const Func &output,
                        const std::map<std::string, Func> &tangents) {
    // Topologically sort the functions
    std::map<std::string, Internal::Function> env =
        Internal::find_transitive_calls(output.function());
    std::vector<std::string> order =
        Internal::realization_order({ output.function() }, env).first;
    std::vector<Func> funcs;
    funcs.reserve(order.size());
    for (const auto &func_name : order) {
        Func func(env[func_name]);
        funcs.push_back(Func(env[func_name]));
    }

    std::vector<Func> transformed_funcs;
    transformed_funcs.reserve(order.size());
    std::map<std::string, Func> updated_tangents = tangents;
    for (const Func &func : funcs) {
        Func transformed_func(func.name() + "_fwd");
        Tuple v = func.values();
        std::vector<Expr> tv;
        for (const Expr &e : v.as_vector()) {
            Expr new_expr = Internal::forward_accumulation(e, updated_tangents);
            //new_expr = print_when(is_nan(new_expr) != 0, new_expr, std::string("NaN founds in ") + transformed_func.name());
            tv.push_back(new_expr);
        }
        transformed_func(func.args()) = Tuple(tv);
        updated_tangents[func.name()] = transformed_func;
        for (int update_id = 0; update_id < func.num_update_definitions(); update_id++) {
            Tuple v = func.update_values(update_id);
            std::vector<Expr> tv;
            for (const Expr &e : v.as_vector()) {
                Expr new_expr = Internal::forward_accumulation(e, updated_tangents);
                //new_expr = print_when(is_nan(new_expr) != 0, new_expr, std::string("NaN founds in ") + transformed_func.name());
                tv.push_back(new_expr);
            }
            transformed_func(func.update_args(update_id)) = Tuple(tv);
            updated_tangents[func.name()] = transformed_func;
        }
        transformed_funcs.push_back(transformed_func);
    }

    return transformed_funcs.back();
}

}  // namespace Halide
