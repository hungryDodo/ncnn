# Copyright 2021 Tencent
# SPDX-License-Identifier: BSD-3-Clause

import torch
import torch.nn as nn
import torch.nn.functional as F

class Model(nn.Module):
    def __init__(self):
        super(Model, self).__init__()

        self.act_0 = nn.ELU()
        self.act_1 = nn.ELU(alpha=1.3)

    def forward(self, x, y, z, w):
        x = x * 2 - 1
        y = y * 2 - 1
        z = z * 2 - 1
        w = w * 2 - 1
        x = self.act_0(x)
        y = self.act_0(y)
        z = self.act_1(z)
        w = self.act_1(w)
        return x, y, z, w

def test():
    net = Model()
    net.eval()

    torch.manual_seed(0)
    x = torch.rand(12)
    y = torch.rand(12, 64)
    z = torch.rand(12, 24, 64)
    w = torch.rand(12, 24, 32, 64)

    a = net(x, y, z, w)

    # export torchscript
    mod = torch.jit.trace(net, (x, y, z, w))
    mod.save("test_nn_ELU.pt")

    # torchscript to pnnx
    import os
    os.system("../../src/pnnx test_nn_ELU.pt inputshape=[12],[12,64],[12,24,64],[12,24,32,64]")

    # ncnn inference
    import test_nn_ELU_ncnn
    b = test_nn_ELU_ncnn.test_inference()

    for a0, b0 in zip(a, b):
        if not torch.allclose(a0, b0, 1e-4, 1e-4):
            return False
    return True

if __name__ == "__main__":
    if test():
        exit(0)
    else:
        exit(1)
