#ifndef CONTROL_RW_H
#define CONTROL_RW_H

#include <vector>

// interfaz base que define las cuatro acciones que debe implementar cualquier controlador de lectores y escritores.
class ControlRW {
public:
    virtual ~ControlRW() = default;
    virtual void entrar_lectura() = 0;
    virtual void salir_lectura() = 0;
    virtual void entrar_escritura() = 0;
    virtual void salir_escritura() = 0;
};

enum class Politica {
    Lectores,
    Escritores,
    Fifo
};

enum class Tipo {
    Lectora,
    Escritora
};

enum class Accion {
    Pide,
    Entra,
    Sale
};

// registro de la traza que guarda que hebra actuo, de que tipo era, que hizo y su turno.
struct AnotacionTraza {
    int hebra;    
    Tipo tipo;
    Accion accion;
    long turno;   
};

// contadores de los distintos tipos de fallos o choques al intentar entrar en modo atomico.
struct FallosAdmision {
    long recurso_ocupado = 0;
    long esc_esperando = 0;
    long cas_fallido = 0;
};

// cada hebra guarda aqui su propio numero de identificacion
extern thread_local int id_hebra;

#endif