target_sources(vectordb PRIVATE
    cpu_features.cpp
    distance.cpp
    kernel_avx2.cpp
    kernel_neon.cpp
    kernel_registry.cpp
    kernel_scalar.cpp
)
