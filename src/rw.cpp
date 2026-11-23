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

// cada hebra guarda aqui su propio numero de identificacion
thread_local int id_hebra = -1;

// doble_version siempre deberia ser el doble de version.
struct Registro {
    long version = 0;
    long doble_version = 0;
};

// contadores para ir anotando lectoras, escritoras y posibles fallos mientras corre el programa.
std::atomic<int> lectoras_activas{0};
std::atomic<int> escritoras_activas{0};
std::atomic<int> max_lectoras{0};
std::atomic<long> inconsistencias{0};
std::atomic<long> violaciones{0};

// actualiza el maximo de lectoras leyendo a la vez si el valor nuevo es mayor. (Compare-And-Swap)
void actualizar_maximo(int actual) {
    int maximo = max_lectoras.load();
    while (actual > maximo &&
           !max_lectoras.compare_exchange_weak(maximo, actual)) {
    }
}

// convierte un texto a numero long y avisa si el texto estaba vacio, tenias letras o era muy grande.
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

// revisa si el texto de la politica es valido y guarda la opcion correspondiente.
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

// anota para cada hebra sus tiempos de espera, el maximo y  saca el promedio.
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

// pide al sistema cuanto tiempo de procesador lleva usando el programa (en nanosegundos).
double tiempo_cpu() {
    timespec ts{};

    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) {
        return -1;
    }

    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

int main(int argc, char* argv[]) {
    // si no pasaron 7 ni 8 parametros en la consola (8 o 9 contando ./rw), muestra como usarlo y sale.
    if (argc != 8 && argc != 9) {
        std::cerr << "Uso:\n"
                  << "./rw <modo> <politica> <L> <E> <K> "
                  << "<dur_lec_us> <dur_esc_us> [notraza]\n";
        return 1;
    }

    std::string modo = argv[1];
    std::string nombre_politica = argv[2];

    // toma el modo y revisa que sea cond o atomic.
    if (modo != "cond" && modo != "atomic") {
        std::cerr << "Error: modo invalido. Use cond o atomic.\n";
        return 1;
    }

    // convierte el texto de la politica y corta el programa si no es valida.
    Politica politica;

    if (!obtener_politica(nombre_politica, politica)) {
        std::cerr << "Error: politica invalida.\n";
        return 1;
    }

    // si el modo es atomic, la politica tiene que ser escritores.
    if (modo == "atomic" && politica != Politica::Escritores) {
        std::cerr << "Error: atomic solo admite la politica escritores.\n";
        return 1;
    }

    // convierte L, E, K y las duraciones a numeros long, si alguno no es valido, muestra error y sale.
    long L, E, K, dur_lec, dur_esc;

    if (!convertir_long(argv[3], L) ||
        !convertir_long(argv[4], E) ||
        !convertir_long(argv[5], K) ||
        !convertir_long(argv[6], dur_lec) ||
        !convertir_long(argv[7], dur_esc)) {
        std::cerr << "Error: los argumentos deben ser numeros validos.\n";
        return 1;
    }

    // revisa que L, E y K sean al menos 1, que el total de hebras no supere 1000 y que las duraciones no sean negativas.
    if (L < 1 || E < 1 || K < 1 ||
        L + E > 1000 ||
        dur_lec < 0 || dur_esc < 0) {
        std::cerr << "Error: parametros fuera de rango.\n";
        return 1;
    }

    // por defecto no se desactiva la traza, si viene un noveno argumento, exige que sea notraza.
    bool notraza = false;
    if (argc == 9) {
        if (std::string(argv[8]) != "notraza") {
            std::cerr << "Error: argumento opcional invalido.\n";
            return 1;
        }
        notraza = true;
    }

    const long MAX = std::numeric_limits<long>::max();

    // revisa con divisiones que 2*E*K y (L+E)*K no se pasen del numero maximo que cabe en un long.
    if (E > MAX / 2 || K > (MAX / 2) / E) {
        std::cerr << "Error: 2*E*K desborda un long.\n";
        return 1;
    }

    if (K > MAX / (L + E)) {
        std::cerr << "Error: (L+E)*K desborda un long.\n";
        return 1;
    }

    // calcula la cantidad total de operaciones que haran todas las hebras juntas.
    long operaciones = (L + E) * K;

    // crea el controlador segun el modo elegido (monitor con condiciones o control atomico) y libera la memoria solo al final.
    std::unique_ptr<ControlRW> control;

    if (modo == "cond") {
        control = std::make_unique<MonitorRW>(politica, !notraza);
    } else {
        control = std::make_unique<ControlAtomicoRW>();
    }

    // crea el registro compartido y un registro de tiempos de espera para cada lectora y cada escritora.
    Registro registro;

    std::vector<TiempoEspera> esperas_lectoras(L);
    std::vector<TiempoEspera> esperas_escritoras(E);

    // prepara la lista donde se guardaran todas las hebras y reserva espacio para L + E.
    std::vector<std::thread> hebras;
    hebras.reserve(L + E);

    // para pausar la hebra actual la cantidad de microsegundos indicada (si es mayor que 0).
    auto dormir = [](long duracion) {
        if (duracion > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(duracion));
        }
    };

    // marca el inicio de los cronometros antes de crear hebras.
    auto inicio_pared = std::chrono::steady_clock::now();
    double inicio_cpu = tiempo_cpu();

    try {
        // crea una hebra lectora por cada indice i de 0 a L - 1.
        for (int i = 0; i < L; i++) {
            // lanza una hebra nueva capturando 'i' por valor para que cada una conserve su propio numero.
            hebras.emplace_back([&, i]() {
                id_hebra = i; // guarda el numero de esta lectora en su variable local de hebra.

                // repite K veces la operacion de lectura.
                for (long k = 0; k < K; k++) {
                    // mide cuanto rato espera la lectora hasta que el control le da permiso para entrar.
                    auto inicio_espera = std::chrono::steady_clock::now();
                    control->entrar_lectura();
                    auto fin_espera = std::chrono::steady_clock::now();

                    double espera = std::chrono::duration<double, std::micro>(
                        fin_espera - inicio_espera).count();
                    esperas_lectoras[i].registrar(espera);
                    
                    // suma 1 a las lectoras que estan leyendo y actualiza el maximo si corresponde.
                    int actuales = lectoras_activas.fetch_add(1) + 1;
                    actualizar_maximo(actuales);
                    
                    // si hay alguna escritora adentro al mismo tiempo, anota una violacion.
                    if (escritoras_activas.load() != 0) {
                        violaciones.fetch_add(1);
                    }

                    // lee version y doble_version, si doble_version no es el doble, suma una inconsistencia.
                    long v = registro.version;
                    long dv = registro.doble_version;

                    if (dv != 2 * v) {
                        inconsistencias.fetch_add(1);
                    }

                    // pausa la hebra los microsegundos indicados para simular el tiempo de lectura.
                    dormir(dur_lec);

                    // salio de leer y avisa al controlador.
                    lectoras_activas.fetch_sub(1);
                    control->salir_lectura();
                }
            });
        }

        // crea E hebras escritoras y les asigna un ID desde L en adelante para no repetir numeros.
        for (int i = 0; i < E; i++) {
            hebras.emplace_back([&, i]() {
                id_hebra = static_cast<int>(L) + i;
                // repite K veces la operacion de escritura.
                for (long k = 0; k < K; k++) {
                    // mide cuanto espera la escritora hasta que el control la deja entrar.
                    auto inicio_espera = std::chrono::steady_clock::now();
                    control->entrar_escritura();
                    auto fin_espera = std::chrono::steady_clock::now();

                    double espera = std::chrono::duration<double, std::micro>(
                        fin_espera - inicio_espera).count();
                    esperas_escritoras[i].registrar(espera);
                    
                    // suma 1 a escritoras_activas y revisa si ya habia otra escritora o lectoras adentro.
                    int anteriores = escritoras_activas.fetch_add(1);

                    if (anteriores != 0 || lectoras_activas.load() != 0) {
                        violaciones.fetch_add(1);
                    }

                    // actualiza version y su doble, espera el tiempo de escritura y avisa al salir.
                    registro.version++;
                    registro.doble_version = 2 * registro.version;

                    // espera el tiempo de escritura, marca que salio y avisa al controlador que ya termino.
                    dormir(dur_esc);

                    escritoras_activas.fetch_sub(1);
                    control->salir_escritura();
                }
            });
        }
    } // si falla al crear una hebra, muestra el error, espera a las que ya se crearon y sale con error.
    catch (const std::system_error& e) {
        std::cerr << "Error al crear una hebra: " << e.what() << "\n";

        for (auto& hebra : hebras) {
            if (hebra.joinable()) {
                hebra.join();
            }
        }

        return 1;
    }

    // espera a que terminen todas las hebras antes de seguir con el calculo final y los resultados.
    for (auto& hebra : hebras) {
        if (hebra.joinable()) {
            hebra.join();
        }
    }
    // toma las marcas finales de tiempo y calcula cuanto duro la prueba en milisegundos (reloj y CPU).
    auto fin_pared = std::chrono::steady_clock::now();
    double fin_cpu = tiempo_cpu();

    double tiempo_pared = std::chrono::duration<double, std::milli>(
        fin_pared - inicio_pared).count();

    double tiempo_cpu_ms = (inicio_cpu >= 0 && fin_cpu >= 0)
        ? (fin_cpu - inicio_cpu) / 1e6
        : -1;
    
    // calcula cuanto deberian valer version y doble_version al final si todas las escrituras salieron bien.
    long version_esperada = E * K;
    long doble_esperada = 2 * E * K;
    
    // marca true solo si los valores finales coinciden con lo esperado y no hubo inconsistencias ni violaciones.
    bool correcto =
        registro.version == version_esperada &&
        registro.doble_version == doble_esperada &&
        inconsistencias.load() == 0 &&
        violaciones.load() == 0;
    
    // crea dos acumuladores generales para juntar las esperas de todas las lectoras y de todas las escritoras.
    TiempoEspera total_lectoras;
    TiempoEspera total_escritoras;
    
    // junta la suma, la cantidad de esperas y el maximo de todas las lectoras, hace lo mismo con las escritoras.
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

    // muestra en consola los resultados finales, tiempos de espera y si el programa salio bien.
    // deja los decimales con 3 cifras despues de la coma y salta una linea antes del reporte.
    std::cout << std::fixed << std::setprecision(3) << "\n";

    // imprime el modo, la politica, la cantidad de lectoras, escritoras, vueltas y total de operaciones.
    std::cout << "modo=" << modo << " politica=" << nombre_politica
              << " L=" << L << " E=" << E << " K=" << K
              << " ops=" << operaciones << "\n";

    // muestra los valores finales de version y su doble, fallos encontrados y el maximo de lectoras juntas.
    std::cout << "version=" << registro.version << "/" << version_esperada
              << " doble_version=" << registro.doble_version << "/" << doble_esperada
              << " inconsistencias=" << inconsistencias.load()
              << " violaciones=" << violaciones.load()
              << " max_lectoras_simultaneas=" << max_lectoras.load() << "\n";
    
    // imprime cuanto duro la prueba y el promedio/maximo de espera de lectoras y escritoras.
    std::cout << "tiempo_pared_ms=" << tiempo_pared
              << " tiempo_cpu_ms=" << tiempo_cpu_ms << "\n";
    std::cout << "espera_lectoras_us: prom=" << total_lectoras.promedio()
              << " max=" << total_lectoras.maximo_us << "\n";
    std::cout << "espera_escritoras_us: prom=" << total_escritoras.promedio()
              << " max=" << total_escritoras.maximo_us << "\n";

    // muestra SI si no hubo fallos ni valores incorrectos al final, o NO en caso contrario.
    std::cout << "correcto=" << (correcto ? "SI" : "NO") << "\n";

    bool todo_ok = correcto; // se asume que todo salio bien si los datos y contadores anteriores no tuvieron fallos.

    // si es modo cond con traza activa, revisa la traza contra la politica y muestra si se cumplio.
    if (modo == "cond" && !notraza) {
        // comprueba que el controlador sea un MonitorRW antes de pedir su traza.
        auto* monitor = dynamic_cast<MonitorRW*>(control.get());
        if (monitor != nullptr) {
            auto traza = monitor->obtener_traza();
            auto resultado = verificar_traza(traza, politica);

            std::cout << "politica_ok=" << (resultado.politica_ok ? "SI" : "NO")
                      << " evidencia=" << resultado.evidencia << "\n";
            
             // si no se respeto la politica, muestra el numero de anotacion y la regla que fallo.
            if (!resultado.politica_ok) {
                std::cout << "fallo en anotacion " << resultado.numero_error
                          << ": " << resultado.regla_error << "\n";
            }

            // si fallo la politica, marca que no salio todo bien.
            todo_ok = todo_ok && resultado.politica_ok;
        }
    }

    // si es modo atomic, pide y muestra cuantos intentos chocaron o fallaron al intentar entrar.
    if (modo == "atomic") {
        auto fallos = static_cast<ControlAtomicoRW*>(control.get())
            ->obtener_contadores_fallos();

        std::cout << "fallos atomic -> recurso_ocupado=" << fallos.recurso_ocupado
                  << " escritoras_esperando=" << fallos.escritoras_esperando
                  << " cas_fallido=" << fallos.cas_fallido << "\n";
    }

    // devuelve 0 si todo salio bien o 1 si hubo algun error o incumplimiento.
    return todo_ok ? 0 : 1;
}