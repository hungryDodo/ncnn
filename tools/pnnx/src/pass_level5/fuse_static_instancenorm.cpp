// Copyright 2022 Tencent
// SPDX-License-Identifier: BSD-3-Clause

#include "fuse_static_instancenorm.h"

#include "pass_level2.h"

#include <math.h>
#include <string.h>

namespace pnnx {

class fuse_static_Finstancenorm_pass_1d : public GraphRewriterPass
{
public:
    const char* match_pattern_graph() const
    {
        return R"PNNXIR(7767517
5 4
pnnx.Input              input       0 1 input
pnnx.Attribute          op_weight   0 1 weight @data=(%num_features)f32
pnnx.Attribute          op_bias     0 1 bias @data=(%num_features)f32
F.instance_norm         op_0        3 1 input weight bias out running_mean=None running_var=None eps=%eps
pnnx.Output             output      1 0 out
)PNNXIR";
    }

    const char* replace_pattern_graph() const
    {
        return R"PNNXIR(7767517
3 2
pnnx.Input              input       0 1 input
nn.InstanceNorm1d       in          1 1 input out num_features=%num_features eps=%eps affine=True track_running_stats=False @weight=%op_weight.data @bias=%op_bias.data
pnnx.Output             output      1 0 out
)PNNXIR";
    }

    bool match(const std::map<std::string, const Operator*>& matched_operators, const std::map<std::string, Parameter>& /*captured_params*/, const std::map<std::string, Attribute>& /*captured_attrs*/) const
    {
        size_t input_rank = matched_operators.at("op_0")->inputs[0]->shape.size();
        return input_rank == 2 || input_rank == 3;
    }
};

class fuse_static_Finstancenorm_pass_2d : public GraphRewriterPass
{
public:
    const char* match_pattern_graph() const
    {
        return R"PNNXIR(7767517
5 4
pnnx.Input              input       0 1 input
pnnx.Attribute          op_weight   0 1 weight @data=(%num_features)f32
pnnx.Attribute          op_bias     0 1 bias @data=(%num_features)f32
F.instance_norm         op_0        3 1 input weight bias out running_mean=None running_var=None eps=%eps #input=(?,?,?,?)f32
pnnx.Output             output      1 0 out
)PNNXIR";
    }

    const char* replace_pattern_graph() const
    {
        return R"PNNXIR(7767517
3 2
pnnx.Input              input       0 1 input
nn.InstanceNorm2d       in          1 1 input out num_features=%num_features eps=%eps affine=True track_running_stats=False @weight=%op_weight.data @bias=%op_bias.data
pnnx.Output             output      1 0 out
)PNNXIR";
    }
};

class fuse_static_Finstancenorm_pass_3d : public GraphRewriterPass
{
public:
    const char* match_pattern_graph() const
    {
        return R"PNNXIR(7767517
5 4
pnnx.Input              input       0 1 input
pnnx.Attribute          op_weight   0 1 weight @data=(%num_features)f32
pnnx.Attribute          op_bias     0 1 bias @data=(%num_features)f32
F.instance_norm         op_0        3 1 input weight bias out running_mean=None running_var=None eps=%eps #input=(?,?,?,?,?)f32
pnnx.Output             output      1 0 out
)PNNXIR";
    }

    const char* replace_pattern_graph() const
    {
        return R"PNNXIR(7767517
3 2
pnnx.Input              input       0 1 input
nn.InstanceNorm3d       in          1 1 input out num_features=%num_features eps=%eps affine=True track_running_stats=False @weight=%op_weight.data @bias=%op_bias.data
pnnx.Output             output      1 0 out
)PNNXIR";
    }
};

void fuse_static_instancenorm(Graph& graph)
{
    fuse_static_Finstancenorm_pass_1d a;
    fuse_static_Finstancenorm_pass_2d b;
    fuse_static_Finstancenorm_pass_3d c;
    int opindex = 0;

    pnnx_graph_rewrite(graph, &a, opindex);
    pnnx_graph_rewrite(graph, &b, opindex);
    pnnx_graph_rewrite(graph, &c, opindex);
}

} // namespace pnnx
