#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "control_atomico_rw.h"
#include "control_rw.h"
#include "monitor_rw.h"
#include "verificador.h"

thread_local int id_hebra = -1;

struct Registro {
    long version = 0;
    long doble_version = 0;
};

std::atomic<int> lectoras_activas{0};
std::atomic<int> escritoras_activas{0};
std::atomic<int> max_lectoras{0};
std::atomic<long> inconsistencias{0};
std::atomic<long> violaciones{0};

void actualizar_maximo(int actual) {
    int maximo = max_lectoras.load();

    while (actual > maximo &&
           !max_lectoras.compare_exchange_weak(maximo, actual)) {
    }
}

bool convertir_long(const char* texto, long& resultado) {
    errno = 0;
    char* fin = nullptr;

    long valor = std::strtol(texto, &fin, 10);

    if (errno == ERANGE || fin == texto || *fin != '\0') {
        return false;
    }

    resultado = valor;
    return true;
}

bool obtener_politica(const std::string& nombre, Politica& p) {
    if (nombre == "lectores") {
        p = Politica::Lectores;
    } else if (nombre == "escritores") {
        p = Politica::Escritores;
    } else if (nombre == "fifo") {
        p = Politica::Fifo;
    } else {
        return false;
    }

    return true;
}

struct TiempoEspera {
    long double suma_us = 0;
    double maximo_us = 0;
    long cantidad = 0;

    void registrar(double tiempo) {
        suma_us += tiempo;
        maximo_us = std::max(maximo_us, tiempo);
        cantidad++;
    }

    double promedio() const {
        if (cantidad == 0) return 0;
        return static_cast<double>(suma_us / cantidad);
    }
};

double tiempo_cpu() {
    timespec ts{};

    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) {
        return -1;
    }

    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

int main(int argc, char* argv[]) {
    if (argc != 8 && argc != 9) {
        std::cerr << "Uso:\n"
                  << "./rw <modo> <politica> <L> <E> <K> "
                  << "<dur_lec_us> <dur_esc_us> [notraza]\n";
        return 1;
    }

    std::string modo = argv[1];
    std::string nombre_politica = argv[2];

    if (modo != "cond" && modo != "atomic") {
        std::cerr << "Error: modo invalido. Use cond o atomic.\n";
        return 1;
    }

    Politica politica;

    if (!obtener_politica(nombre_politica, politica)) {
        std::cerr << "Error: politica invalida.\n";
        return 1;
    }

    if (modo == "atomic" && politica != Politica::Escritores) {
        std::cerr << "Error: atomic solo admite la politica escritores.\n";
        return 1;
    }

    long L, E, K, dur_lec, dur_esc;

    if (!convertir_long(argv[3], L) ||
        !convertir_long(argv[4], E) ||
        !convertir_long(argv[5], K) ||
        !convertir_long(argv[6], dur_lec) ||
        !convertir_long(argv[7], dur_esc)) {
        std::cerr << "Error: los argumentos deben ser numeros validos.\n";
        return 1;
    }

    if (L < 1 || E < 1 || K < 1 ||
        L + E > 1000 ||
        dur_lec < 0 || dur_esc < 0) {
        std::cerr << "Error: parametros fuera de rango.\n";
        return 1;
    }

    bool notraza = false;

    if (argc == 9) {
        if (std::string(argv[8]) != "notraza") {
            std::cerr << "Error: argumento opcional invalido.\n";
            return 1;
        }
        notraza = true;
    }

    const long MAX = std::numeric_limits<long>::max();

    if (E > MAX / 2 || K > (MAX / 2) / E) {
        std::cerr << "Error: 2*E*K desborda un long.\n";
        return 1;
    }

    if (K > MAX / (L + E)) {
        std::cerr << "Error: (L+E)*K desborda un long.\n";
        return 1;
    }

    long operaciones = (L + E) * K;

    std::unique_ptr<ControlRW> control;

    if (modo == "cond") {
        control = std::make_unique<MonitorRW>(politica, !notraza);
    } else {
        control = std::make_unique<ControlAtomicoRW>();
    }

    Registro registro;

    std::vector<TiempoEspera> esperas_lectoras(L);
    std::vector<TiempoEspera> esperas_escritoras(E);

    std::vector<std::thread> hebras;
    hebras.reserve(L + E);

    auto dormir = [](long duracion) {
        if (duracion > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(duracion));
        }
    };

    auto inicio_pared = std::chrono::steady_clock::now();
    double inicio_cpu = tiempo_cpu();

    try {
        for (int i = 0; i < L; i++) {
            hebras.emplace_back([&, i]() {
                id_hebra = i;

                for (long k = 0; k < K; k++) {
                    auto inicio_espera = std::chrono::steady_clock::now();
                    control->entrar_lectura();
                    auto fin_espera = std::chrono::steady_clock::now();

                    double espera = std::chrono::duration<double, std::micro>(
                        fin_espera - inicio_espera).count();
                    esperas_lectoras[i].registrar(espera);

                    int actuales = lectoras_activas.fetch_add(1) + 1;
                    actualizar_maximo(actuales);

                    if (escritoras_activas.load() != 0) {
                        violaciones.fetch_add(1);
                    }

                    long v = registro.version;
                    long dv = registro.doble_version;

                    if (dv != 2 * v) {
                        inconsistencias.fetch_add(1);
                    }

                    dormir(dur_lec);

                    lectoras_activas.fetch_sub(1);
                    control->salir_lectura();
                }
            });
        }

        for (int i = 0; i < E; i++) {
            hebras.emplace_back([&, i]() {
                id_hebra = static_cast<int>(L) + i;

                for (long k = 0; k < K; k++) {
                    auto inicio_espera = std::chrono::steady_clock::now();
                    control->entrar_escritura();
                    auto fin_espera = std::chrono::steady_clock::now();

                    double espera = std::chrono::duration<double, std::micro>(
                        fin_espera - inicio_espera).count();
                    esperas_escritoras[i].registrar(espera);

                    int anteriores = escritoras_activas.fetch_add(1);

                    if (anteriores != 0 || lectoras_activas.load() != 0) {
                        violaciones.fetch_add(1);
                    }

                    registro.version++;
                    registro.doble_version = 2 * registro.version;

                    dormir(dur_esc);

                    escritoras_activas.fetch_sub(1);
                    control->salir_escritura();
                }
            });
        }
    } catch (const std::system_error& e) {
        std::cerr << "Error al crear una hebra: " << e.what() << "\n";

        for (auto& hebra : hebras) {
            if (hebra.joinable()) {
                hebra.join();
            }
        }

        return 1;
    }

    for (auto& hebra : hebras) {
        if (hebra.joinable()) {
            hebra.join();
        }
    }

    auto fin_pared = std::chrono::steady_clock::now();
    double fin_cpu = tiempo_cpu();

    double tiempo_pared = std::chrono::duration<double, std::milli>(
        fin_pared - inicio_pared).count();

    double tiempo_cpu_ms = (inicio_cpu >= 0 && fin_cpu >= 0)
        ? (fin_cpu - inicio_cpu) / 1e6
        : -1;

    long version_esperada = E * K;
    long doble_esperada = 2 * E * K;

    bool correcto =
        registro.version == version_esperada &&
        registro.doble_version == doble_esperada &&
        inconsistencias.load() == 0 &&
        violaciones.load() == 0;

    TiempoEspera total_lectoras;
    TiempoEspera total_escritoras;

    for (const auto& e : esperas_lectoras) {
        total_lectoras.suma_us += e.suma_us;
        total_lectoras.cantidad += e.cantidad;
        total_lectoras.maximo_us = std::max(total_lectoras.maximo_us, e.maximo_us);
    }

    for (const auto& e : esperas_escritoras) {
        total_escritoras.suma_us += e.suma_us;
        total_escritoras.cantidad += e.cantidad;
        total_escritoras.maximo_us = std::max(total_escritoras.maximo_us, e.maximo_us);
    }

    std::cout << std::fixed << std::setprecision(3) << "\n";
    std::cout << "modo=" << modo << " politica=" << nombre_politica
              << " L=" << L << " E=" << E << " K=" << K
              << " ops=" << operaciones << "\n";
    std::cout << "version=" << registro.version << "/" << version_esperada
              << " doble_version=" << registro.doble_version << "/" << doble_esperada
              << " inconsistencias=" << inconsistencias.load()
              << " violaciones=" << violaciones.load()
              << " max_lectoras_simultaneas=" << max_lectoras.load() << "\n";
    std::cout << "tiempo_pared_ms=" << tiempo_pared
              << " tiempo_cpu_ms=" << tiempo_cpu_ms << "\n";
    std::cout << "espera_lectoras_us: prom=" << total_lectoras.promedio()
              << " max=" << total_lectoras.maximo_us << "\n";
    std::cout << "espera_escritoras_us: prom=" << total_escritoras.promedio()
              << " max=" << total_escritoras.maximo_us << "\n";
    std::cout << "correcto=" << (correcto ? "SI" : "NO") << "\n";

    bool todo_ok = correcto;

    if (modo == "cond" && !notraza) {
        auto* monitor = dynamic_cast<MonitorRW*>(control.get());
        if (monitor != nullptr) {
            auto traza = monitor->obtener_traza();
            auto resultado = verificar_traza(traza, politica);

            std::cout << "politica_ok=" << (resultado.politica_ok ? "SI" : "NO")
                      << " evidencia=" << resultado.evidencia << "\n";

            if (!resultado.politica_ok) {
                std::cout << "fallo en anotacion " << resultado.numero_error
                          << ": " << resultado.regla_error << "\n";
            }

            todo_ok = todo_ok && resultado.politica_ok;
        }
    }

    if (modo == "atomic") {
        auto fallos = static_cast<ControlAtomicoRW*>(control.get())
            ->obtener_contadores_fallos();

        std::cout << "fallos atomic -> recurso_ocupado=" << fallos.recurso_ocupado
                  << " escritoras_esperando=" << fallos.escritoras_esperando
                  << " cas_fallido=" << fallos.cas_fallido << "\n";
    }

    return todo_ok ? 0 : 1;
}