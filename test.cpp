#include <iostream>
#include "nvEncodeAPI.h" // The header you found

int main() {
    // This is a struct defined in nvEncodeAPI.h
    NV_ENCODE_API_FUNCTION_LIST functionList = { NV_ENCODE_API_FUNCTION_LIST_VER };
    
    // This is the function the .lib file provides a bridge to
    NVENCSTATUS status = NvEncodeAPICreateInstance(&functionList);

    if (status != NV_ENC_SUCCESS) {
        std::cerr << "NVIDIA Encoder check failed. (Is a GPU driver installed?)" << std::endl;
        return 1;
    }

    std::cout << "Success! The NVENCODE API is ready." << std::endl;
    return 0;
}