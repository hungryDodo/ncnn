# Copyright 2022 Tencent
# SPDX-License-Identifier: BSD-3-Clause

import torch
import torch.nn as nn
import torch.nn.functional as F

class Model(nn.Module):
    def __init__(self):
        super(Model, self).__init__()

        self.act_0 = nn.Softmax2d()

    def forward(self, x):
        x = x * 2 - 1
        x = self.act_0(x)
        return x

def test():
    net = Model()
    net.eval()

    torch.manual_seed(0)
    x = torch.rand(1, 12, 24, 64)

    a = net(x)

    # export torchscript
    mod = torch.jit.trace(net, x)
    mod.save("test_nn_Softmax2d.pt")

    # torchscript to pnnx
    import os
    os.system("../src/pnnx test_nn_Softmax2d.pt inputshape=[1,12,24,64]")

    # pnnx inference
    import test_nn_Softmax2d_pnnx
    b = test_nn_Softmax2d_pnnx.test_inference()

    return torch.equal(a, b)

if __name__ == "__main__":
    if test():
        exit(0)
    else:
        exit(1)
