# Copyright 2021 Tencent
# SPDX-License-Identifier: BSD-3-Clause

import torch
import torch.nn as nn
import torch.nn.functional as F

class Model(nn.Module):
    def __init__(self):
        super(Model, self).__init__()

        self.rnn_0_0 = nn.RNN(input_size=32, hidden_size=16)
        self.rnn_0_1 = nn.RNN(input_size=16, hidden_size=16, num_layers=3, nonlinearity='tanh', bias=False)
        self.rnn_0_2 = nn.RNN(input_size=16, hidden_size=16, num_layers=4, nonlinearity='tanh', bias=True, bidirectional=True)
        self.rnn_0_3 = nn.RNN(input_size=16, hidden_size=16, num_layers=4, nonlinearity='tanh', bias=True, bidirectional=True)
        self.rnn_0_4 = nn.RNN(input_size=16, hidden_size=16, num_layers=4, nonlinearity='tanh', bias=True, bidirectional=True)

        self.rnn_1_0 = nn.RNN(input_size=25, hidden_size=16, batch_first=True)
        self.rnn_1_1 = nn.RNN(input_size=16, hidden_size=16, num_layers=3, nonlinearity='tanh', bias=False, batch_first=True)
        self.rnn_1_2 = nn.RNN(input_size=16, hidden_size=16, num_layers=4, nonlinearity='tanh', bias=True, batch_first=True, bidirectional=True)
        self.rnn_1_3 = nn.RNN(input_size=16, hidden_size=16, num_layers=4, nonlinearity='tanh', bias=True, batch_first=True, bidirectional=True)
        self.rnn_1_4 = nn.RNN(input_size=16, hidden_size=16, num_layers=4, nonlinearity='tanh', bias=True, batch_first=True, bidirectional=True)

    def forward(self, x, y):
        x = x.permute(1, 0, 2)

        x0, _ = self.rnn_0_0(x)
        x1, _ = self.rnn_0_1(x0)
        x2, h0 = self.rnn_0_2(x1)
        x3, h1 = self.rnn_0_3(x1, h0)
        x4, _ = self.rnn_0_4(x1, h1)

        y0, _ = self.rnn_1_0(y)
        y1, _ = self.rnn_1_1(y0)
        y2, h2 = self.rnn_1_2(y1)
        y3, h3 = self.rnn_1_3(y1, h2)
        y4, _ = self.rnn_1_3(y1, h3)

        x2 = x2.permute(1, 0, 2)
        x3 = x3.permute(1, 0, 2)
        x4 = x4.permute(1, 0, 2)

        return x2, x3, x4, y2, y3, y4

def test():
    net = Model().half().float()
    net.eval()

    torch.manual_seed(0)
    x = torch.rand(1, 10, 32)
    y = torch.rand(1, 12, 25)

    a = net(x, y)

    # export torchscript
    mod = torch.jit.trace(net, (x, y))
    mod.save("test_nn_RNN.pt")

    # torchscript to pnnx
    import os
    os.system("../../src/pnnx test_nn_RNN.pt inputshape=[1,10,32],[1,12,25]")

    # ncnn inference
    import test_nn_RNN_ncnn
    b = test_nn_RNN_ncnn.test_inference()

    for a0, b0 in zip(a, b):
        if not torch.allclose(a0, b0, 1e-3, 1e-3):
            return False
    return True

if __name__ == "__main__":
    if test():
        exit(0)
    else:
        exit(1)
