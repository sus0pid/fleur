add_executable(fleur_benchmark
    fleur_benchmark.c
)

target_link_libraries(fleur_benchmark PRIVATE
    fleur
)
