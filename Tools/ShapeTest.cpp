// 快速测试：Kim_Vocal_2.onnx 的时间轴是否接受 dim_t < 256（决定实时方案）
#include <onnxruntime_cxx_api.h>
#include <cstdio>
#include <vector>

int main()
{
    setvbuf (stdout, nullptr, _IONBF, 0);
    Ort::Env env (ORT_LOGGING_LEVEL_WARNING, "ShapeTest");
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads (1);
    Ort::Session session (env, L"D:\\Xiangmu\\S-Rack\\ThirdParty\\Kim_Vocal_2.onnx", opts);

    for (int dimT : { 256, 128, 64, 32 })
    {
        try
        {
            std::vector<int64_t> shape { 1, 4, 3072, dimT };
            std::vector<float> data ((size_t) 4 * 3072 * dimT, 0.0f);
            auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
            Ort::Value in (Ort::Value::CreateTensor<float> (mem, data.data(), data.size(), shape.data(), 4));
            const char* inName = "input";
            const char* outName = "output";
            auto outs = session.Run (Ort::RunOptions { nullptr }, &inName, &in, 1, &outName, 1);
            auto info = outs[0].GetTensorTypeAndShapeInfo();
            auto dims = info.GetShape();
            std::printf ("dimT=%d -> OK, out dims=[", dimT);
            for (auto d : dims) std::printf ("%lld ", (long long) d);
            std::printf ("]\n");
        }
        catch (const Ort::Exception& e)
        {
            std::printf ("dimT=%d -> FAIL: %s\n", dimT, e.what());
        }
    }
    return 0;
}
