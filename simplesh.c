/*
 * Shell `simplesh` (basado en el shell de xv6)
 *
 * Ampliación de Sistemas Operativos
 * Departamento de Ingeniería y Tecnología de Computadores
 * Facultad de Informática de la Universidad de Murcia
 *
 * Alumnos: García Martínez, Alejandro (G2.1)
 *          Pérez Martínez, Eduardo (G2.2)
 *
 * Convocatoria: FEBRERO
 */


/*
 * Ficheros de cabecera
 */

// bloquear sigchild antes del fork y desbloquearla después de salir de el o bien antes del run_cmd bloquear y desbloquear despues del run_cmd
// el codigo para bloquear e ignorar una señal (b4) se debe añadir justo al inicio del programa

#define _POSIX_C_SOURCE 200809L /* IEEE 1003.1-2008 (véase /usr/include/features.h) */
//#define NDEBUG                /* Traduce asertos y DMACROS a 'no ops' */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
//getuid/pwuid
#include <sys/types.h>
#include <pwd.h>
//getcwd
#include <limits.h>
//basename
#include <libgen.h>
// Biblioteca readline
#include <readline/readline.h>
#include <readline/history.h>
// cwd
#include <unistd.h>
//psplit
#include <ctype.h>
//señales
#include <signal.h>
#include <math.h>
//man 2 llamadas al sistema y man 3 funciones de biblioteca

/******************************************************************************
 * Constantes, macros y variables globales
 ******************************************************************************/
const int BSIZEMAX = 1048576;
static const int intersNumber = 5;
static const char* internCommands[] = {"cwd","exit", "cd", "psplit", "bjobs"};
static const char* VERSION = "0.19";
int oldpwddefined = 0;
#define NMAXPROCCES 31
int childsproc[NMAXPROCCES];
int childs_act = 0;
sigset_t blocked_signals ;

// Niveles de depuración
#define DBG_CMD   (1 << 0)
#define DBG_TRACE (1 << 1)
// . . .
static int g_dbg_level = 0;

#ifndef NDEBUG
#define DPRINTF(dbg_level, fmt, ...)                            \
    do {                                                        \
        if (dbg_level & g_dbg_level)                            \
            fprintf(stderr, "%s:%d:%s(): " fmt,                 \
                    __FILE__, __LINE__, __func__, ##__VA_ARGS__);       \
    } while ( 0 )

#define DBLOCK(dbg_level, block)                                \
    do {                                                        \
        if (dbg_level & g_dbg_level)                            \
            block;                                              \
    } while( 0 );
#else
#define DPRINTF(dbg_level, fmt, ...)
#define DBLOCK(dbg_level, block)
#endif

#define TRY(x)                                                  \
    do {                                                        \
        int __rc = (x);                                         \
        if( __rc < 0 ) {                                        \
            fprintf(stderr, "%s:%d:%s: TRY(%s) failed\n",       \
                    __FILE__, __LINE__, __func__, #x);          \
            fprintf(stderr, "ERROR: rc=%d errno=%d (%s)\n",     \
                    __rc, errno, strerror(errno));              \
            exit(EXIT_FAILURE);                                 \
        }                                                       \
    } while( 0 )


// Número máximo de argumentos de un comando
#define MAX_ARGS 16


// Delimitadores
static const char WHITESPACE[] = " \t\r\n\v";
// Caracteres especiales
static const char SYMBOLS[] = "<|>&;()";

/******************************************************************************
 * Funciones auxiliares
 ******************************************************************************/


// Imprime el mensaje
void info(const char *fmt, ...)
{
    va_list arg;

    fprintf(stdout, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stdout, fmt, arg);
    va_end(arg);
}


// Imprime el mensaje de error
void error(const char *fmt, ...)
{
    va_list arg;

    fprintf(stderr, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);
}


// Imprime el mensaje de error y aborta la ejecución
void panic(const char *fmt, ...)
{
    va_list arg;

    fprintf(stderr, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);

    exit(EXIT_FAILURE);
}


// `fork()` que muestra un mensaje de error si no se puede crear el hijo
int fork_or_panic(const char* s)
{
    int pid;

    pid = fork();
    if(pid == -1)
        panic("%s failed: errno %d (%s)", s, errno, strerror(errno));
    return pid;
}


/******************************************************************************
 * Estructuras de datos `cmd`
 ******************************************************************************/


// Las estructuras `cmd` se utilizan para almacenar información que servirá a
// simplesh para ejecutar líneas de órdenes con redirecciones, tuberías, listas
// de comandos y tareas en segundo plano. El formato es el siguiente:

//     |----------+--------------+--------------|
//     | (1 byte) | ...          | ...          |
//     |----------+--------------+--------------|
//     | type     | otros campos | otros campos |
//     |----------+--------------+--------------|

// Nótese cómo las estructuras `cmd` comparten el primer campo `type` para
// identificar su tipo. A partir de él se obtiene un tipo derivado a través de
// *casting* forzado de tipo. Se consigue así polimorfismo básico en C.

// Valores del campo `type` de las estructuras de datos `cmd`
enum cmd_type { EXEC=1, REDR=2, PIPE=3, LIST=4, BACK=5, SUBS=6, INV=7 };

struct cmd { enum cmd_type type; };

// Comando con sus parámetros
struct execcmd {
    enum cmd_type type;
    char* argv[MAX_ARGS];
    char* eargv[MAX_ARGS];
    int argc;
};

// Comando con redirección
struct redrcmd {
    enum cmd_type type;
    struct cmd* cmd;
    char* file;
    char* efile;
    int flags;
    mode_t mode;
    int fd;
};

// Comandos con tubería
struct pipecmd {
    enum cmd_type type;
    struct cmd* left;
    struct cmd* right;
};

// Lista de órdenes
struct listcmd {
    enum cmd_type type;
    struct cmd* left;
    struct cmd* right;
};

// Tarea en segundo plano (background) con `&`
struct backcmd {
    enum cmd_type type;
    struct cmd* cmd;
};

// Subshell
struct subscmd {
    enum cmd_type type;
    struct cmd* cmd;
};


/******************************************************************************
 * Funciones para construir las estructuras de datos `cmd`
 ******************************************************************************/


// Construye una estructura `cmd` de tipo `EXEC`
struct cmd* execcmd(void)
{
    struct execcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        
        perror("execcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = EXEC;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `REDR`
struct cmd* redrcmd(struct cmd* subcmd,
        char* file, char* efile,
        int flags, mode_t mode, int fd)
{
    struct redrcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("redrcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = REDR;
    cmd->cmd = subcmd;
    cmd->file = file;
    cmd->efile = efile;
    cmd->flags = flags;
    cmd->mode = mode;
    cmd->fd = fd;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `PIPE`
struct cmd* pipecmd(struct cmd* left, struct cmd* right)
{
    struct pipecmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("pipecmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = PIPE;
    cmd->left = left;
    cmd->right = right;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `LIST`
struct cmd* listcmd(struct cmd* left, struct cmd* right)
{
    struct listcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("listcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = LIST;
    cmd->left = left;
    cmd->right = right;

    return (struct cmd*)cmd;
}

// Construye una estructura `cmd` de tipo `BACK`
struct cmd* backcmd(struct cmd* subcmd)
{
    struct backcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("backcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = BACK;
    cmd->cmd = subcmd;

    return (struct cmd*)cmd;
}

// Construye una estructura `cmd` de tipo `SUB`
struct cmd* subscmd(struct cmd* subcmd)
{
    struct subscmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("subscmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = SUBS;
    cmd->cmd = subcmd;

    return (struct cmd*) cmd;
}


/******************************************************************************
 * Funciones para realizar el análisis sintáctico de la línea de órdenes
 ******************************************************************************/


// `get_token` recibe un puntero al principio de una cadena (`start_of_str`),
// otro puntero al final de esa cadena (`end_of_str`) y, opcionalmente, dos
// punteros para guardar el principio y el final del token, respectivamente.
//
// `get_token` devuelve un *token* de la cadena de entrada.

int get_token(char** start_of_str, char const* end_of_str,
        char** start_of_token, char** end_of_token)
{
    char* s;
    int ret;

    // Salta los espacios en blanco
    s = *start_of_str;
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;

    // `start_of_token` apunta al principio del argumento (si no es NULL)
    if (start_of_token)
        *start_of_token = s;

    ret = *s;
    switch (*s)
    {
        case 0:
            break;
        case '|':
        case '(':
        case ')':
        case ';':
        case '&':
        case '<':
            s++;
            break;
        case '>':
            s++;
            if (*s == '>')
            {
                ret = '+';
                s++;
            }
            break;

        default:

            // El caso por defecto (cuando no hay caracteres especiales) es el
            // de un argumento de un comando. `get_token` devuelve el valor
            // `'a'`, `start_of_token` apunta al argumento (si no es `NULL`),
            // `end_of_token` apunta al final del argumento (si no es `NULL`) y
            // `start_of_str` avanza hasta que salta todos los espacios
            // *después* del argumento. Por ejemplo:
            //
            //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
            //     | (espacio) | a | r | g | u | m | e | n | t | o | (espacio)
            //     |
            //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
            //                   ^                                   ^
            //            start_o|f_token                       end_o|f_token

            ret = 'a';
            while (s < end_of_str &&
                    !strchr(WHITESPACE, *s) &&
                    !strchr(SYMBOLS, *s))
                s++;
            break;
    }

    // `end_of_token` apunta al final del argumento (si no es `NULL`)
    if (end_of_token)
        *end_of_token = s;

    // Salta los espacios en blanco
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;

    // Actualiza `start_of_str`
    *start_of_str = s;

    return ret;
}


// `peek` recibe un puntero al principio de una cadena (`start_of_str`), otro
// puntero al final de esa cadena (`end_of_str`) y un conjunto de caracteres
// (`delimiter`).
//
// El primer puntero pasado como parámero (`start_of_str`) avanza hasta el
// primer carácter que no está en el conjunto de caracteres `WHITESPACE`.
//
// `peek` devuelve un valor distinto de `NULL` si encuentra alguno de los
// caracteres en `delimiter` justo después de los caracteres en `WHITESPACE`.

int peek(char** start_of_str, char const* end_of_str, char* delimiter)
{
    char* s;

    s = *start_of_str;
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;
    *start_of_str = s;

    return *s && strchr(delimiter, *s);
}


// Definiciones adelantadas de funciones
struct cmd* parse_line(char**, char*);
struct cmd* parse_pipe(char**, char*);
struct cmd* parse_exec(char**, char*);
struct cmd* parse_subs(char**, char*);
struct cmd* parse_redr(struct cmd*, char**, char*);
struct cmd* null_terminate(struct cmd*);


// `parse_cmd` realiza el *análisis sintáctico* de la línea de órdenes
// introducida por el usuario.
//
// `parse_cmd` utiliza `parse_line` para obtener una estructura `cmd`.

struct cmd* parse_cmd(char* start_of_str)
{
    char* end_of_str;
    struct cmd* cmd;

    DPRINTF(DBG_TRACE, "STR\n");

    end_of_str = start_of_str + strlen(start_of_str);

    cmd = parse_line(&start_of_str, end_of_str);

    // Comprueba que se ha alcanzado el final de la línea de órdenes
    peek(&start_of_str, end_of_str, "");
    if (start_of_str != end_of_str)
        error("%s: error sintáctico: %s\n", __func__);

    DPRINTF(DBG_TRACE, "END\n");

    return cmd;
}


// `parse_line` realiza el análisis sintáctico de la línea de órdenes
// introducida por el usuario.
//
// `parse_line` comprueba en primer lugar si la línea contiene alguna tubería.
// Para ello `parse_line` llama a `parse_pipe` que a su vez verifica si hay
// bloques de órdenes y/o redirecciones.  A continuación, `parse_line`
// comprueba si la ejecución de la línea se realiza en segundo plano (con `&`)
// o si la línea de órdenes contiene una lista de órdenes (con `;`).

struct cmd* parse_line(char** start_of_str, char* end_of_str)
{
    struct cmd* cmd;
    int delimiter;

    cmd = parse_pipe(start_of_str, end_of_str);

    while (peek(start_of_str, end_of_str, "&"))
    {
        // Consume el delimitador de tarea en segundo plano
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '&');

        // Construye el `cmd` para la tarea en segundo plano
        cmd = backcmd(cmd);
    }

    if (peek(start_of_str, end_of_str, ";"))
    {
        if (cmd->type == EXEC && ((struct execcmd*) cmd)->argv[0] == 0)
            error("%s: error sintáctico: no se encontró comando\n", __func__);

        // Consume el delimitador de lista de órdenes
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == ';');

        // Construye el `cmd` para la lista
        cmd = listcmd(cmd, parse_line(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_pipe` realiza el análisis sintáctico de una tubería de manera
// recursiva si encuentra el delimitador de tuberías '|'.
//
// `parse_pipe` llama a `parse_exec` y `parse_pipe` de manera recursiva para
// realizar el análisis sintáctico de todos los componentes de la tubería.

struct cmd* parse_pipe(char** start_of_str, char* end_of_str)
{
    struct cmd* cmd;
    int delimiter;

    cmd = parse_exec(start_of_str, end_of_str);

    if (peek(start_of_str, end_of_str, "|"))
    {
        if (cmd->type == EXEC && ((struct execcmd*) cmd)->argv[0] == 0)
            error("%s: error sintáctico: no se encontró comando\n", __func__);

        // Consume el delimitador de tubería
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '|');

        // Construye el `cmd` para la tubería
        cmd = pipecmd(cmd, parse_pipe(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_exec` realiza el análisis sintáctico de un comando a no ser que la
// expresión comience por un paréntesis, en cuyo caso se llama a `parse_subs`.
//
// `parse_exec` reconoce las redirecciones antes y después del comando.

struct cmd* parse_exec(char** start_of_str, char* end_of_str)
{
    char* start_of_token;
    char* end_of_token;
    int token, argc;
    struct execcmd* cmd;
    struct cmd* ret;

    // ¿Inicio de un bloque?
    if (peek(start_of_str, end_of_str, "("))
        return parse_subs(start_of_str, end_of_str);

    // Si no, lo primero que hay en una línea de órdenes es un comando

    // Construye el `cmd` para el comando
    ret = execcmd();
    cmd = (struct execcmd*) ret;

    // ¿Redirecciones antes del comando?
    ret = parse_redr(ret, start_of_str, end_of_str);

    // Bucle para separar los argumentos de las posibles redirecciones
    argc = 0;
    while (!peek(start_of_str, end_of_str, "|)&;"))
    {
        if ((token = get_token(start_of_str, end_of_str,
                        &start_of_token, &end_of_token)) == 0)
            break;

        // El siguiente token debe ser un argumento porque el bucle
        // para en los delimitadores
        if (token != 'a')
            error("%s: error sintáctico: se esperaba un argumento\n", __func__);

        // Almacena el siguiente argumento reconocido. El primero es
        // el comando
        cmd->argv[argc] = start_of_token;
        cmd->eargv[argc] = end_of_token;
        cmd->argc = ++argc;
        if (argc >= MAX_ARGS)
            panic("%s: demasiados argumentos\n", __func__);

        // ¿Redirecciones después del comando?
        ret = parse_redr(ret, start_of_str, end_of_str);
    }

    // El comando no tiene más parámetros
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;
    return ret;
}


// `parse_subs` realiza el análisis sintáctico de un bloque de órdenes
// delimitadas por paréntesis o `subshell` llamando a `parse_line`.
//
// `parse_subs` reconoce las redirecciones después del bloque de órdenes.

struct cmd* parse_subs(char** start_of_str, char* end_of_str)
{
    int delimiter;
    struct cmd* cmd;
    struct cmd* scmd;

    // Consume el paréntesis de apertura
    if (!peek(start_of_str, end_of_str, "("))
        error("%s: error sintáctico: se esperaba '('", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == '(');

    // Realiza el análisis sintáctico hasta el paréntesis de cierre
    scmd = parse_line(start_of_str, end_of_str);

    // Construye el `cmd` para el bloque de órdenes
    cmd = subscmd(scmd);

    // Consume el paréntesis de cierre
    if (!peek(start_of_str, end_of_str, ")"))
        error("%s: error sintáctico: se esperaba ')'", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == ')');

    // ¿Redirecciones después del bloque de órdenes?
    cmd = parse_redr(cmd, start_of_str, end_of_str);

    return cmd;
}


// `parse_redr` realiza el análisis sintáctico de órdenes con
// redirecciones si encuentra alguno de los delimitadores de
// redirección ('<' o '>').

struct cmd* parse_redr(struct cmd* cmd, char** start_of_str, char* end_of_str)
{
    int delimiter;
    char* start_of_token;
    char* end_of_token;

    // Si lo siguiente que hay a continuación es delimitador de
    // redirección...
    while (peek(start_of_str, end_of_str, "<>"))
    {
        // Consume el delimitador de redirección
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '<' || delimiter == '>' || delimiter == '+');

        // El siguiente token tiene que ser el nombre del fichero de la
        // redirección entre `start_of_token` y `end_of_token`.
        if ('a' != get_token(start_of_str, end_of_str, &start_of_token, &end_of_token))
            error("%s: error sintáctico: se esperaba un fichero", __func__);

        // Construye el `cmd` para la redirección
        switch(delimiter)
        {
            case '<':
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_RDONLY, S_IRWXU, STDIN_FILENO);  // permisos 700
                break;
            case '>':
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_WRONLY|O_CREAT|O_TRUNC, S_IRWXU, STDOUT_FILENO);
                break;
            case '+': // >>
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_WRONLY|O_CREAT|O_APPEND, S_IRWXU, STDOUT_FILENO);
                break;
        }
    }

    return cmd;
}


// Termina en NULL todas las cadenas de las estructuras `cmd`
struct cmd* null_terminate(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct pipecmd* pcmd;
    struct listcmd* lcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;
    int i;

    if(cmd == 0)
        return 0;

    switch(cmd->type)
    {
        case EXEC:
            ecmd = (struct execcmd*) cmd;
            for(i = 0; ecmd->argv[i]; i++)
                *ecmd->eargv[i] = 0;
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            null_terminate(rcmd->cmd);
            *rcmd->efile = 0;
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;
            null_terminate(pcmd->left);
            null_terminate(pcmd->right);
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;
            null_terminate(lcmd->left);
            null_terminate(lcmd->right);
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;
            null_terminate(bcmd->cmd);
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            null_terminate(scmd->cmd);
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }

    return cmd;
}


/******************************************************************************
 * Lectura de la línea de órdenes con la biblioteca libreadline
 ******************************************************************************/


// `get_cmd` muestra un *prompt* y lee lo que el usuario escribe usando la
// biblioteca readline. Ésta permite mantener el historial, utilizar las flechas
// para acceder a las órdenes previas del historial, búsquedas de órdenes, etc.

char* get_cmd()
{
    // buscamos el usuario
    uid_t user = getuid();
    struct passwd* username = getpwuid(user);
    if (!username){
        perror("getpwuid");
        exit(EXIT_FAILURE);   
    }
    char* usuario = username->pw_name;
    //printf("%s", usuario);

    char path[PATH_MAX];
    if (! getcwd(path, PATH_MAX)){
        perror("getcwd");
        exit(EXIT_FAILURE);
    }
    //printf("%s", path);  //ruta absoluta
    char * dir = basename(path);
    //printf("%s", dir); // directorio actual
    //char* ruta;
    //ruta = getwd();
    //printf("cwd: %s", ruta);
    char prompt[strlen(usuario) + strlen(dir) + 4];
    sprintf(prompt, "%s@%s> ", usuario, dir);
    // Lee la orden tecleada por el usuario
    char * buf = readline(prompt);

    // Si el usuario ha escrito una orden, almacenarla en la historia.
    if(buf)
        add_history(buf);

    return buf;
}


/******************************************************************************
 * Funciones para la ejecución de la línea de órdenes
 ******************************************************************************/


void exec_cmd(struct execcmd* ecmd)
{
    assert(ecmd->type == EXEC);

    if (ecmd->argv[0] == NULL) exit(EXIT_SUCCESS);

    execvp(ecmd->argv[0], ecmd->argv);

    panic("no se encontró el comando '%s'\n", ecmd->argv[0]);
}

void run_cwd(){
    char path[PATH_MAX];
    if (!getcwd(path,PATH_MAX)){
        perror("getcwd at run_cwd\n");
        exit(EXIT_FAILURE);
    }
    printf("cwd: %s\n", path);
}

void run_cd(struct execcmd* ecmd){
    int argc = ecmd->argc;
    //char* argv[MAX_ARGS] = ecmd->argv;
    optind = 1;
    char* path[argc];
    int length = 0;
    int i;
    for (i = optind ; i < argc ; i++){
        path[i - 1] = (ecmd->argv[i]);
        length++;
    }
    
    if (length > 1){
        printf("run_cd: Demasiados argumentos\n");
        return;
    }
    // caso de "cd ", vamos al directorio almacenado en la variable de entorno $HOME
    if (length == 0){
        char* home = getenv("HOME");
        char* pwd = getenv("PWD");
        //char* oldpwd;
        //= getenv("OLDPWD");
        //printf("$HOME = %s\n$PWD = %s\n$OLDPWD=%s\n", home, pwd,oldpwd);
        setenv("OLDPWD", pwd, 1);
        oldpwddefined = 1;
        setenv("PWD", home, 1);
        chdir(home);
        //printf("$HOME = %s\n$PWD = %s\n$OLDPWD=%s\n", getenv("HOME"), getenv("PWD"), getenv("OLDPWD"));
    // caso de volvemos al directorio anterior
    } else if (length == 1 && *path[0] == '-'){
        char* pwd = getenv("PWD");
        char* oldpwd = getenv("OLDPWD");
        if (oldpwd != NULL && oldpwddefined != 0){
    //printf("futuro pwd: %s, oldpwd:%s\n", pwd, oldpwd);
            if (chdir(oldpwd)) printf("error\n");
            oldpwddefined = 1;
            setenv("OLDPWD", pwd, 1);
            setenv("PWD", oldpwd, 1);
        } else {
            printf("run_cd: Variable OLDPWD no definida\n");
            return;
        }
        
    } 
    else {
        char * destino = *path;
        if (chdir(destino)){
            printf("run_cd: No existe el directorio '%s'\n", destino);
            return;
        }
        oldpwddefined = 1;
        char current[PATH_MAX];
        setenv("OLDPWD", getenv("PWD"), 1);
        setenv("PWD", getcwd(current, sizeof(current)), 1);
        //printf("$HOME = %s\n$PWD = %s\n$OLDPWD=%s\n", getenv("HOME"), getenv("PWD"), getenv("OLDPWD"));
    }
}
// mira si una cadena es estrictamente numerica y si lo es devuelve 0, si no lo es devuelve 1
int String_is_a_digit(char* ch){
    for (int i = 0; ch[i] != '\0'; i++){
        if (! isdigit(ch[i])){
            //printf("No es un numero\n");
            return 1;
        }
    }
    return 0;
}

void run_psplit(struct execcmd* ecmd){
    int opt , flag_l, flag_b, flag_s, flag_p, flag_h , lines, size, bytesfich, procs;
    flag_l = flag_b = flag_p =flag_s = flag_h = 0;
    int argc = ecmd->argc;
    char* argv[argc];
    for (int i = 0; i<argc; i++) argv[i] = ecmd->argv[i];
    char* ficheros_entrada[argc];
    int numero_ficheros_entrada = 0;
    optind = 0;
    while (( opt = getopt ( ecmd->argc, ecmd->argv , "l:b:p:s:h")) != -1) {
        switch (opt)
        {
        case 'l':
            //printf("l flag\n");
            flag_l = 1;
            if (optarg == NULL){
                printf("psplit: invalid number of aruments for -l\n");
                return;
            } else {
                //vemos el numero de lineas en que queremos dividir (parametro obligatorio)
                if (String_is_a_digit(optarg)){
                    printf("invalid number of lines for -l <<%s>>\n", optarg);
                    return;
                }
                lines = atoi(optarg);
                //printf("%s  %d\n",ecmd->argv[i],lines);
                //mirar si es un numero valido
                if (lines <= 0){
                    printf("invalid number of lines for -l <<%s>>\n", optarg);
                    return;
                }
                //printf("lines %d", lines);
            }
            break;

        case 'b':
            flag_b = 1;
            //TODO mirar si hay un numero o algo que mirar despues para evitar el core
            if (optarg == NULL){
                printf("psplit: invalid number of aruments for -b\n");
                return;
            } else {
                if (String_is_a_digit(optarg)){
                    printf("invalid number of bytes for -b <<%s>>\n",optarg);
                    return;
                }
                bytesfich = atoi(optarg);
                //mirar si es un numero valido
                if (bytesfich <= 0){
                    printf("invalid number of bytes for -b <<%s>>\n",optarg);
                    return;
                }
            }
            break;

        case 's':
            flag_s = 1;
            //miramos si hay parametro
            //TODO mirar si hay un numero o algo que mirar despues para evitar el core
            if (optarg == NULL){
                printf("psplit: invalid number of aruments for -s\n");
                return;
            } else {
                if (String_is_a_digit(optarg)){
                    printf("psplit: Opción -s no válida\n");
                    return;
                }
                size = atoi(optarg);
                //mirar si es un numero valido
                if (size <= 0 || size > BSIZEMAX){
                    printf("psplit: Opción -s no válida\n");
                    return;
                }
            }
            break;

        case 'p':
            flag_p = 1;
            if (optarg == NULL){
                printf("psplit: invalid number of aruments for -p\n");
                return;
            } else {
                if (String_is_a_digit(optarg)){
                    printf("psplit: Opción -p no válida\n");
                    return;
                }
                procs = atoi(optarg);
                //mirar si es un numero valido
                if (procs <= 0){
                    printf("psplit: Opción -p no válida\n");
                    return;
                }
            }
            break;

        case 'h':
            flag_h;
            //mostrar ayuda y parar, no se hace nada mas
            printf("Uso: psplit [-l NLINES] [-b NBYTES] [-s BSIZE] [-p PROCS] [FILE1] [FILE2]...\n");
            printf("\tOpciones:\n");
            printf("\t-l NLINES Número máximo de líneas por fichero.\n");
            printf("\t-b NBYTES Número máximo de bytes por fichero.\n");
            printf("\t-s BSIZE Tamaño en bytes de los bloques leídos de [FILEn] o stdin.\n");
            printf("\t-p PROCS Número máximo de procesos simultáneos.\n");
            printf("\t-h        Ayuda\n\n");
            return;
        default:
            printf("options requieres after functions '-l', '-b', '-s' and '-p'\n");
            break;
        }
    }
    //hemos terminado de procesar los parametros
    if (flag_b && flag_l){
        //printf("psplit: parameters \"-l\" and \"b\" are incompatible\n");
        printf("psplit: Opciones incompatibles\n");
        return;
    }
    if (flag_p == 0){
        procs = 1;
    }
    if (flag_b == 0 && flag_l == 0){
        bytesfich = 1024;
    }
    if (flag_s == 0){
        size = 1024;
    }
    //pasamos a leer los ficheros de entrada y salida
    int i = 0;
    int flags = flag_b+flag_l+flag_p+flag_s;
    for (; optind < argc ; optind++){
        if(strcmp(argv[optind], ecmd ->argv[optind]) != 0) return;
        ficheros_entrada[i] = ecmd->argv[optind];
        i++;
    }
    //si no habia ningun fichero tenemos que coger la entrada estandar
    int cogemos_entrada_estandar = 0;
    if (i==0){
        cogemos_entrada_estandar = 1;
        ficheros_entrada[0]= "stdin";
        i++;
    }
    //ya tenemos los ficheros a leer para splitear
    int terminado = 0;
    int paralelos = 0;
    int fich_leidos = 0;
    int fich_act = 0;
    int pidPadre = getpid();
    int pidHIjos[procs];
    int frente, cola;
    frente = cola = 0;
    while (terminado == 0){
        if (fich_leidos >= i) {
            terminado = 1;
            break;
        }
        if (paralelos < procs){
            fich_leidos++;
            paralelos++;
            int id = fork();
            if (getpid() != pidPadre){
                //cada hijo abre un fichero para splitear
                int fich = 0;
                if (cogemos_entrada_estandar == 1)
                    dup2(STDIN_FILENO, fich);
                else
                    fich = open(ficheros_entrada[fich_act], O_RDONLY);
                if (fich < 0) {
                    printf("Permision denied for acces the fich <%s>\n", ficheros_entrada[fich_act]);
                    exit(0);
                }
                //reservamos memoria para el buffer de lectura
                char buffer[size];
                //memset(buffer,0, sizeof(buffer));
                //leemos
                int leidos = 0;
                int numfich = 0;
                leidos = read(fich,buffer,size);
                int datos_res_en_el_buffer = leidos;
                //printf("se han leido %d bytes, quedan %d: %s\n", leidos, datos_res_en_el_buffer, buffer);
                while (leidos > 0){
                    //creamos el fichero si no existe y lo preparamos para escribir en el
                    int escribir;
                    char toWrite[PATH_MAX];
                    sprintf(toWrite, "%s%d",basename(ficheros_entrada[fich_act]), numfich);
                    //printf("vamos a escribir en <%s>\n", toWrite);
                    //printf("fichero %s\n", toWrite);
                    escribir = open(toWrite,O_CREAT | O_WRONLY | O_TRUNC, S_IRWXU);
                    if (escribir < 0) {
                        printf("Permision denied for acces the fich <%s>\n", toWrite);
                        exit(0);
                        }
                    if (datos_res_en_el_buffer == 0){
                        //si no nos quedan datos en el buffer, leemos y actualizamos contadores
                        leidos = read(fich,buffer,size);
                        if (leidos <0 ) exit(1);
                        datos_res_en_el_buffer = leidos;
                    }
                    if (flag_l == 0){
                        //printf("flag l no está activo\n");
                        //una vez abierto, vemos cuantos bytes podemos escribir
                        //esta variable indica cuantos bytes podemos escribir en el fichero todavia
                        int por_escribir_en_el_fich = bytesfich;
                        //printf("quedan por escribir en el fichero %d bytes\n", por_escribir_en_el_fich);
                        //mientras no hayamos completado el fichero
                        while (por_escribir_en_el_fich > 0 && leidos > 0){
                            
                            //printf("me queda por escribir %d y tengo en el buffer %d\n", por_escribir_en_el_fich, datos_res_en_el_buffer);
                            if (datos_res_en_el_buffer > 0){
                                //miramos por donde vamos leyendo del buffer
                                int comienzo_a_escribir_desde = leidos - datos_res_en_el_buffer;
                                //printf("me he quedado por la posicion %d del buffer\n", comienzo_a_escribir_desde);
                                char * pos_buff ;
                                pos_buff = &(buffer[comienzo_a_escribir_desde]);
                                int he_escrito;
                                if (datos_res_en_el_buffer > por_escribir_en_el_fich){//printf("%d en el buffer, %d por escribir\n",datos_res_en_el_buffer, por_escribir_en_el_fich);
                                    he_escrito = write(escribir, pos_buff, por_escribir_en_el_fich);
                                } else {
                                    he_escrito = write(escribir, pos_buff, datos_res_en_el_buffer);
                                }
                                //miro cuanto puedo escribir  en el fichero y lo escribo
                                if (he_escrito < 0) exit(1);
                                por_escribir_en_el_fich -= he_escrito;
                                datos_res_en_el_buffer -= he_escrito;
                                //printf("me queda por escribir %d y tengo en el buffer %d\n", por_escribir_en_el_fich, datos_res_en_el_buffer);

                            } else {
                                if (leidos <0 ) exit(1);
                                leidos = read(fich,buffer,size);
                                datos_res_en_el_buffer = leidos;
                                //printf("leo %d mas, que son: %s\n", leidos, buffer);
                            }
                        }
                        
                    } else {
                        // tratar para numero de lineas
                        int lineas_restantes = lines;
                        while (lineas_restantes > 0 && leidos > 0){
                            if (datos_res_en_el_buffer > 0){
                                //vemos cuanto hemos avanzado en el buffer hasta ahora
                                int comienzo_a_escribir_desde = leidos - datos_res_en_el_buffer;
                                //printf("me he quedado por la posicion %d del buffer\n", comienzo_a_escribir_desde);
                                char * pos_buff ;
                                //y nos colocamos en esa posicion
                                pos_buff = &(buffer[comienzo_a_escribir_desde]);
                                //buscamos desde esa posición cuantas líneas podamos escribir
                                int k = 0;
                                for (; k < datos_res_en_el_buffer && lineas_restantes>0;k++){
                                    if (pos_buff[k] == '\n' || pos_buff[k] == '\r'){
                                        lineas_restantes--;
                                    }
                                }
                                //escribimos hasta que hemos podido leer
                                int he_escrito = write(escribir, pos_buff, k);
                                if (he_escrito < 0) exit(1);
                                datos_res_en_el_buffer -= he_escrito;
                            } else {
                                leidos = read(fich,buffer,size);
                                if (leidos <0 ) exit(1);
                                datos_res_en_el_buffer = leidos;
                                //printf("leo %d mas, que son: %s\n", leidos, buffer);
                            }
                        }

                    }
                    //leemos de nuevo del fichero si fuera necesario y actualizamos para el siguiente fichero a crear
                    numfich++;
                    if (datos_res_en_el_buffer == 0){
                        leidos = read(fich,buffer,size);
                        if (leidos < 0) exit(1);
                        datos_res_en_el_buffer = leidos;
                    }
                    TRY(fsync(escribir));
                    TRY(close(escribir));
                }
                // fsync del fichero al escribir
                TRY(close(fich));
                exit(0);
            } else {
                // pasamos al siguiente fichero
                fich_act++;
                //como somos el padre lo metemos en la cola para esperarlos por orden
                pidHIjos[cola] = id;
                if (cola == procs-1)
                    cola = 0;
                else cola++;
                //printf("meto el hijo %d con pid %d\n", fich_act, id);
                //for (int x = 0; x < procs; x++) printf("%d | ", pidHIjos[x]);
                //printf("\n frente %d, cola %d\n", frente, cola);
            }
        } else {
            // procedemos a sacar el hijo del frente
            waitpid(pidHIjos[frente],NULL,0);
            //waitid(P_PID,pidHIjos[frente],siginfo_t.si_pid ,WEXITED);
            //printf("saco el hijo %d con pid %d\n", fich_act, pidHIjos[frente]);
            if (frente == procs-1)
                frente = 0;
            else frente++;
            //for (int x = 0; x < procs; x++) printf("%d | ", pidHIjos[x]);
            //printf("\n frente %d, cola %d\n", frente, cola);
            paralelos--;
        }
    }
    for (int j=0; j < paralelos;){
        waitpid(pidHIjos[frente],NULL,0);
        //printf("saco el hijo %d con pid %d\n", fich_act, pidHIjos[frente]);
        if (frente == procs-1)
            frente = 0;
        else frente++;
            // for (int x = 0; x < procs; x++) printf("%d | ", pidHIjos[x]);
            // printf("\n frente %d, cola %d\n", frente, cola);
        paralelos--;
    }
}

//muestra / mata todos los procesos en segundo plano
void run_bjobs(struct execcmd* ecmd){
    int opt , flag_k;
    flag_k = 0;
    optind = 0;
    while (( opt = getopt ( ecmd->argc, ecmd->argv , "kh")) != -1) {
        switch (opt){
            case 'h':
                printf("USO:  bjobs [-k] [-h]\n");
                printf("      Opciones:\n");
                printf("      -k Mata todos los procesos en segundo plano.\n");
                printf("      -h Ayuda\n");
                return;
            case 'k':
                flag_k = 1;
                break;
            default:
                break;
        }
    }
    // procesadas todas las opciones
    if (flag_k == 0){
        //mostramos todos los procesos
        //printf("\n");
        for (int i = 0; i<NMAXPROCCES;i++){
            if (childsproc[i] != 0) printf("[%d]\n",childsproc[i]);
        }
    } else {
         for (int i = 0; i<NMAXPROCCES;i++){
            if (childsproc[i] != 0){
                kill(childsproc[i], SIGTERM);
            }
         }
    }
}

int exponecial(int x, int y){
    int r = 1;
    for (int i = 0; i<y; i++){
        r*=x;
    }
    return r;
}

int hashCode(int procID, int r){
    //suponemos que no habrá ningun proceso con id con mas de 100 digitos
    if (r > NMAXPROCCES) {
        perror("max number of proccess reached, couldn't create a new one\n");
        exit(0);
    }
    char texto[100];
    sprintf(texto, "%d",procID);
    int n = strlen(texto);
    int h = 0;
    for (int i = 0; i<n;i++){
        int k = (texto[i] - '0');
        //h += k * pow(31,n-1-i);//no me linkea la libreria matematica :)
        h += k * exponecial(31,n-1-i);
    }
    return (h-r)%NMAXPROCCES;
}

void insertProc(int procID){
    int insertado = 0;
    int r = 0;
    while(insertado == 0){
        int pos = hashCode(procID, r);
        if (childsproc[pos] == 0){
            childsproc[pos] = procID;
            insertado = 1;
            childs_act++;
        } else r++;
    }
}

int getAndRemovProc(int procID){
    int recuperado = 0;
    int r = 0;
    while(recuperado == 0){
        int pos = hashCode(procID, r);
        if (childsproc[pos] == procID){
            childsproc[pos] = 0;
            childs_act--;
            return 1;
        } else r++;
    }
    return 0;
}

//Devuelve si un comando dado es interno o no
int is_intern_command_answerSing(char* command){
    if (command == NULL) return -1;
    for (int i = 0; i < intersNumber; i++){
        if (strcmp(command, internCommands[i]) == 0){
            return i;
        }
    }
    return -1;
}

void print_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;

    if(cmd == 0) return;

    switch (cmd->type){
        case EXEC:
            ecmd = (struct execcmd*) cmd;
        // if (is_intern_command_answerSing(ecmd->argv[0]) >= 0){
        //         printf("intern command detected");
        //     }
        //     else {
            if (ecmd->argv[0] != 0)
                printf("fork( exec( %s ) )", ecmd->argv[0]);
        //    }
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            printf("fork( ");
            if (rcmd->cmd->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) rcmd->cmd)->argv[0]);
            else
                print_cmd(rcmd->cmd);
            printf(" )");
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;
            print_cmd(lcmd->left);
            printf(" ; ");
            print_cmd(lcmd->right);
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;
            printf("fork( ");
            if (pcmd->left->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) pcmd->left)->argv[0]);
            else
                print_cmd(pcmd->left);
            printf(" ) => fork( ");
            if (pcmd->right->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) pcmd->right)->argv[0]);
            else
                print_cmd(pcmd->right);
            printf(" )");
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;
            printf("fork( ");
            if (bcmd->cmd->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) bcmd->cmd)->argv[0]);
            else
                print_cmd(bcmd->cmd);
            printf(" )");
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            printf("fork( ");
            print_cmd(scmd->cmd);
            printf(" )");
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }
}

void free_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            free_cmd(rcmd->cmd);

            free(rcmd->cmd);
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;

            free_cmd(lcmd->left);
            free_cmd(lcmd->right);

            free(lcmd->right);
            free(lcmd->left);
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;

            free_cmd(pcmd->left);
            free_cmd(pcmd->right);

            free(pcmd->right);
            free(pcmd->left);
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;

            free_cmd(bcmd->cmd);

            free(bcmd->cmd);
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;

            free_cmd(scmd->cmd);

            free(scmd->cmd);
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }
}

/*void run_exit(struct cmd* cmd){  
    free_cmd(cmd);
    free(cmd);
    // free(buf);
    exit(0);
}*/

int run_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;
    int p[2];
    int fd;

    DPRINTF(DBG_TRACE, "STR\n");

    if(cmd == 0) return 0;
    sigset_t blocked_chld ;
    sigemptyset (& blocked_chld );
    sigaddset (& blocked_chld , SIGCHLD );
    switch(cmd->type)
    {
        case EXEC:
        //compruebo si es externo o interno, si es externo ejecuto esto de abajo
        if ( sigprocmask ( SIG_BLOCK , & blocked_chld , &blocked_signals ) == -1) {
            perror (" sigprocmask ");
            exit ( EXIT_FAILURE ) ;
        }
            ecmd = (struct execcmd*) cmd;
            int commandInternID = is_intern_command_answerSing(ecmd->argv[0]);
            if (commandInternID >= 0){
                //printf("intern command detected\n");
                switch (commandInternID)
                {
                case 0:
                    run_cwd();
                    break;
                case 1:
                    return 1; 
                    //run_exit(cmd);
                    break;
                case 2: 
                    run_cd(ecmd);
                    break;
                case 3:
                    run_psplit(ecmd);
                    break;
                case 4:
                    run_bjobs(ecmd);
                    break;
                default:
                    break;
                }
            }
            else {
                int pid_hijo_exec;
                if ((pid_hijo_exec = fork_or_panic("fork EXEC")) == 0)
                    exec_cmd(ecmd);
                TRY( waitpid(pid_hijo_exec,NULL,0) );
            }
            if ( sigprocmask ( SIG_UNBLOCK , & blocked_chld , 0 ) == -1) {
            perror (" sigprocmask ");
            exit ( EXIT_FAILURE ) ;
        }
            break;

        case REDR:
        if ( sigprocmask ( SIG_BLOCK , & blocked_chld , &blocked_signals ) == -1) {
            perror (" sigprocmask ");
            exit ( EXIT_FAILURE ) ;
        }
            rcmd = (struct redrcmd*) cmd;
            //comprobar aqui si el comando es interno
            ecmd = (struct execcmd*) rcmd->cmd;
            int commandInternIDREDR = is_intern_command_answerSing(ecmd->argv[0]);
            if (commandInternIDREDR >= 0){
                //int oldfd = dup(STDIN_FILENO);
                //TRY(dup(STDOUT_FILENO));
                int guardado = dup(1);
                close(1);
                //TRY( close(rcmd->fd) );
                    if ((open(rcmd->file, rcmd->flags, rcmd->mode)) < 0)
                    {
                        perror("open");
                        exit(EXIT_FAILURE);
                    }
            //printf("intern command detected\n");
                switch (commandInternIDREDR)
                {
                    case 0:
                        run_cwd();
                        break;
                    case 1: 
                        return 1;
                        //run_exit(cmd);
                        break;
                    case 2: 
                        run_cd(ecmd);
                        break;
                    case 3:
                        run_psplit(ecmd);
                        break;
                    case 4:
                        run_bjobs(ecmd);
                        break;
                    default:
                        break;
                        }
                //dup y dup2 para volver a la salida por pantalla
                close(1);
                dup(guardado);
                close(guardado);
                //TRY(close(fd));
            } else {
                int pid_hijo_redr;
                if ((pid_hijo_redr = fork_or_panic("fork REDR")) == 0)
                {
                    TRY( close(rcmd->fd) );
                    if ((fd = open(rcmd->file, rcmd->flags, rcmd->mode)) < 0)
                    {
                        perror("open");
                        exit(EXIT_FAILURE);
                    }

                    if (rcmd->cmd->type == EXEC)
                        exec_cmd((struct execcmd*) rcmd->cmd);
                    else
                        run_cmd(rcmd->cmd);
                    exit(EXIT_SUCCESS);
                }
                TRY( waitpid(pid_hijo_redr,NULL,0) );
                //TRY( close(rcmd->fd) );
            }
            if ( sigprocmask ( SIG_UNBLOCK , & blocked_chld , 0 ) == -1) {
            perror (" sigprocmask ");
            exit ( EXIT_FAILURE ) ;
        }
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;

            if ( sigprocmask ( SIG_BLOCK , & blocked_chld , &blocked_signals ) == -1) {
            perror (" sigprocmask ");
            exit ( EXIT_FAILURE ) ;
        }
            if (run_cmd(lcmd->left)) return 1;
            if ( sigprocmask ( SIG_UNBLOCK , & blocked_chld , 0 ) == -1) {
            perror (" sigprocmask ");
            exit ( EXIT_FAILURE ) ;
        }

            if ( sigprocmask ( SIG_BLOCK , & blocked_chld , &blocked_signals ) == -1) {
            perror (" sigprocmask ");
            exit ( EXIT_FAILURE ) ;
        }
            if (run_cmd(lcmd->right)) return 1;
            if ( sigprocmask ( SIG_UNBLOCK , & blocked_chld , 0 ) == -1) {
            perror (" sigprocmask ");
            exit ( EXIT_FAILURE ) ;
        }
            break;

        case PIPE:
        if ( sigprocmask ( SIG_BLOCK , & blocked_chld , &blocked_signals ) == -1) {
            perror (" sigprocmask ");
            exit ( EXIT_FAILURE ) ;
        }
            pcmd = (struct pipecmd*)cmd;
            if (pipe(p) < 0)
            {
                perror("pipe");
                exit(EXIT_FAILURE);
            }
            int pid_der, pid_izq;
            // Ejecución del hijo de la izquierda
            if ((pid_izq = fork_or_panic("fork PIPE left")) == 0)
            {
                TRY( close(STDOUT_FILENO) );
                TRY( dup(p[1]) );
                TRY( close(p[0]) );
                TRY( close(p[1]) );
                if (pcmd->left->type == EXEC){
                    ecmd = (struct execcmd*) pcmd->left;
                    int commandInternIDPIPE = is_intern_command_answerSing(ecmd->argv[0]);
                    if (commandInternIDPIPE >= 0){
                        //printf("intern command detected\n");
                        switch (commandInternIDPIPE)
                        {
                            case 0:
                                run_cwd();
                                break;
                            case 1:
                                return 1;
                                //run_exit(cmd);
                                break;
                            case 2: 
                                run_cd(ecmd);
                                break;
                            case 3:
                                run_psplit(ecmd);
                                break;
                            case 4:
                                run_bjobs(ecmd);
                                break;
                            default:
                                break;
                        }
                    } else {
                        exec_cmd((struct execcmd*) pcmd->left);
                    }
                }
                else
                    run_cmd(pcmd->left);
                exit(EXIT_SUCCESS);
            }

            // Ejecución del hijo de la 
            // poner aqui los comandos internos, antes del fork
            if ((pid_der = fork_or_panic("fork PIPE right")) == 0)
            {
                TRY( close(STDIN_FILENO) );
                TRY( dup(p[0]) );
                TRY( close(p[0]) );
                TRY( close(p[1]) );
                if (pcmd->right->type == EXEC){
                    ecmd = (struct execcmd*) pcmd->right;
                    int commandInternIDPIPE = is_intern_command_answerSing(ecmd->argv[0]);
                    if (commandInternIDPIPE >= 0){
                        //printf("intern command detected\n");
                        switch (commandInternIDPIPE)
                        {
                            case 0:
                                run_cwd();
                                break;
                            case 1:
                                return 1; 
                                //run_exit(cmd);
                                break;
                            case 2: 
                                run_cd(ecmd);
                                break;
                            case 3:
                                run_psplit(ecmd);
                                break;
                            case 4:
                                run_bjobs(ecmd);
                                break;
                            default:
                                break;
                        }
                    } else{
                        exec_cmd((struct execcmd*) pcmd->right);
                    }
                }
                else
                    run_cmd(pcmd->right);
                exit(EXIT_SUCCESS);
            }
            TRY( close(p[0]) );
            TRY( close(p[1]) );

            // Esperar a ambos hijos
            TRY( waitpid(pid_izq, NULL, 0) );
            TRY( waitpid(pid_der, NULL, 0) );
            if ( sigprocmask ( SIG_UNBLOCK , & blocked_chld , 0 ) == -1) {
            perror (" sigprocmask ");
            exit ( EXIT_FAILURE ) ;
        }
            break;

        case BACK:
        if ( sigprocmask ( SIG_BLOCK , & blocked_chld , &blocked_signals ) == -1) {
            perror (" sigprocmask ");
            exit ( EXIT_FAILURE ) ;
        }
            bcmd = (struct backcmd*)cmd;
            int pid_hijo_back;
            if ((pid_hijo_back =fork_or_panic("fork BACK")) == 0)
            {
                if (bcmd->cmd->type == EXEC){
                //comprobar si es interno aqui
                    ecmd = (struct execcmd*) bcmd->cmd;
                    int commandInternIDBACK = is_intern_command_answerSing(ecmd->argv[0]);
                    if (commandInternIDBACK >= 0){
                        //printf("intern command detected\n");
                        switch (commandInternIDBACK)
                        {
                            case 0:
                                run_cwd();
                                break;
                            case 1: 
                                return 1;
                                //run_exit(cmd);
                                break;
                            case 2: 
                                run_cd(ecmd);
                                break;
                            case 3:
                                run_psplit(ecmd);
                                break;
                            case 4:
                                run_bjobs(ecmd);
                                break;
                            default:
                                break;
                        }
                    } else {
                        exec_cmd((struct execcmd*) bcmd->cmd);
                    }
                }
                else
                    run_cmd(bcmd->cmd);
                exit(EXIT_SUCCESS);
            }
            //mostrar el pid del hijo creado
            insertProc(pid_hijo_back);
            printf("[%d]\n", pid_hijo_back);
            // for (int i = 0; i<NMAXPROCCES ;i++){
            //     printf("%d |", childsproc[i]);
            // }
            // printf("\n");
            //lo almacenamos en la lista de procesos hijo pendientes
            if ( sigprocmask ( SIG_UNBLOCK , & blocked_chld , 0 ) == -1) {
            perror (" sigprocmask ");
            exit ( EXIT_FAILURE ) ;
        }
            break;

        case SUBS:
        if ( sigprocmask ( SIG_BLOCK , & blocked_chld , &blocked_signals ) == -1) {
            perror (" sigprocmask ");
            exit ( EXIT_FAILURE ) ;
        }
            scmd = (struct subscmd*) cmd;
            int pid_hijo_subs;
            if ((pid_hijo_subs = fork_or_panic("fork SUBS")) == 0)
            {
                run_cmd(scmd->cmd);
                exit(EXIT_SUCCESS);
            }
            TRY( waitpid(pid_hijo_subs, NULL, 0) );
            if ( sigprocmask ( SIG_UNBLOCK , & blocked_chld , 0 ) == -1) {
            perror (" sigprocmask ");
            exit ( EXIT_FAILURE ) ;
        }
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }

    DPRINTF(DBG_TRACE, "END\n");
    return 0;
}
/******************************************************************************
 * Bucle principal de `simplesh`
 ******************************************************************************/


void help(char **argv)
{
    info("Usage: %s [-d N] [-h]\n\
         shell simplesh v%s\n\
         Options: \n\
         -d set debug level to N\n\
         -h help\n\n",
         argv[0], VERSION);
}


void parse_args(int argc, char** argv)
{
    int option;

    // Bucle de procesamiento de parámetros
    while((option = getopt(argc, argv, "d:h")) != -1) {
        switch(option) {
            case 'd':
                g_dbg_level = atoi(optarg);
                break;
            case 'h':
            default:
                help(argv);
                exit(EXIT_SUCCESS);
                break;
        }
    }
}
static void signal_handler ( int sig ){
    // /* SIGINT (^C): advise */
    // if ( sig == SIGINT ) {
    //     printf("sigint incoming\n");
    //     return ;
    // }
    /* SIGQUIT (^\) :  ignore*/
    if (sig == SIGQUIT){
        //printf("sigquit incoming\n");
        return;
    }
}

static void childs_handler( int sig ){
    int saved_errno = errno;
    pid_t pid;
    while ((pid = waitpid((pid_t)(-1), 0, WNOHANG)) > 0){
        getAndRemovProc(pid);
        printf("\n[%d]\n", pid);
    }
    errno = saved_errno;
}

int main(int argc, char** argv)
{   
    sigemptyset (& blocked_signals );
    sigaddset (& blocked_signals , SIGINT );
    if ( sigprocmask ( SIG_BLOCK , & blocked_signals , NULL ) == -1) {
        perror (" sigprocmask ");
        exit ( EXIT_FAILURE ) ;
    }

    struct sigaction sa;
    memset (& sa , 0, sizeof ( sa )) ;
    sa.sa_handler = signal_handler;
    sigemptyset (& sa . sa_mask ) ;
    if ( sigaction ( SIGQUIT , &sa , NULL ) == -1) {
        perror (" sigaction sigquit\n");
        exit ( EXIT_FAILURE ) ;
    }

    memset(childsproc, 0, sizeof(childsproc));
    struct sigaction sac;
    sac.sa_handler = childs_handler;
    //sa restart ->Provide behavior compatible with BSD signal semantics by making certain system calls restartable across signals
    //SA_NOCLDSTOP -> If signum is SIGCHLD, do not receive notification when child processes stop 
    //SA_NOCLDWAIT -> If signum is SIGCHLD, do not transform children into zombies when they terminate
    //sac.sa_flags = SA_RESTART | SA_NOCLDSTOP | SA_NOCLDWAIT;
    sac.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    //sac.sa_flags = SA_NOCLDSTOP;
    //sac.sa_flags = 0;
    if (sigaction(SIGCHLD, &sac, 0) == -1) {
        perror("sigaction sigchld\n");
        exit(1);
    }

    char* buf;
    struct cmd* cmd;
    parse_args(argc, argv);

    DPRINTF(DBG_TRACE, "STR\n");
    int terminado = 0;
    int fin = 0;

    // Bucle de lectura y ejecución de órdenes
    while (!terminado && (buf = get_cmd()) != NULL)
    {
        // Realiza el análisis sintáctico de la línea de órdenes
        cmd = parse_cmd(buf);

        // Termina en `NULL` todas las cadenas de las estructuras `cmd`
        null_terminate(cmd);

        DBLOCK(DBG_CMD, {
            info("%s:%d:%s: print_cmd: ",
                 __FILE__, __LINE__, __func__);
            print_cmd(cmd); printf("\n"); fflush(NULL); } );

        // Ejecuta la línea de órdenes
        //bloqueamos las señales de sigchld mientras estemos ejecutando comandos para no mezclar
        // sigset_t blocked_chld ;
        // sigemptyset (& blocked_chld );
        // sigaddset (& blocked_chld , SIGCHLD );
        // if ( sigprocmask ( SIG_BLOCK , & blocked_chld , &blocked_signals ) == -1) {
        //     perror (" sigprocmask ");
        //     exit ( EXIT_FAILURE ) ;
        // }
        fin = run_cmd(cmd);
        //desbloqueamos sigchld para volver a recibirlas
        // if ( sigprocmask ( SIG_UNBLOCK , & blocked_chld , 0 ) == -1) {
        //     perror (" sigprocmask ");
        //     exit ( EXIT_FAILURE ) ;
        // }

        // Libera la memoria de las estructuras `cmd`
        free_cmd(cmd);
        free(cmd);

        // Libera la memoria de la línea de órdenes
        free(buf);
        terminado = fin;
    }

    DPRINTF(DBG_TRACE, "END\n");

    return 0;
}