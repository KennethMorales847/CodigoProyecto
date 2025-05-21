#include "pch.h"                             // Inicializaci�n de encabezado precompilado para acelerar compilaci�n en Visual Studio
#include <iostream>                          // Librer�a para entrada/salida por consola (ej, std::cin, std::cout)
#include <fstream>                           // Librer�a para manejar archivos (ej, std::ifstream, std::ofstream)
#include <string>                            // Libreria para usar std::string
#include <sstream>                           // Libreria para usar streams basados en cadena (ej, std::istringstream)
#include <map>                               // Librer�a para usar std::map
#include <vector>                            // Libreria para usar std::vector
#include <algorithm>                         // Libreria para algoritmos (ej, std::find, std::transform)
#include <cctype>                            // Libreria para usar funciones de clasificaci�n de caracteres (ej, std::tolower, std::ispunct)
#include <unordered_set>                     // Libreria para usar std::unordered_set (que utilizamos para las stopwords)
#include <curl/curl.h>                       // Libreria para usar libcurl y poder realizar peticiones HTTP a OpenAI

// Inicializar SQL Server
#using <System.Data.dll>                     // Utilizando ensamblado ADO.NET para acceder a bases de datos
#include <msclr/marshal_cppstd.h>            // Libreria utilizada para convertir entre System::String y std::string
using namespace System;                      // Tipos b�sicos .NET
using namespace System::Data;                 // Espacio de datos .NET
using namespace System::Data::SqlClient;      // Proveedores SQL Server
using namespace msclr::interop;               // marshaling entre C++ y .NET

using namespace std;                          // utilizar funciones sin tener que escribir std::

// ---------------------- Colores ANSI para consola ----------------------
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define GRAY    "\033[90m"
#define RESET   "\033[0m"


// ---------------------- Normalizaci�n ----------------------
// Convierte acentos y � a sus equivalentes base para evitar errores de comprension o impresion
char foldAccent(char c) {
    switch ((unsigned char)c) {
    case '\xC1': case '\xE1': return 'a';   // � / � -> a
    case '\xC9': case '\xE9': return 'e';   // � / � -> e
    case '\xCD': case '\xED': return 'i';   // � / � -> i
    case '\xD3': case '\xF3': return 'o';   // � / � -> o
    case '\xDA': case '\xFA': return 'u';   // � / � -> u
    case '\xD1': case '\xF1': return 'n';   // � / � -> n
    default: return c;                        // Otros caracteres se quedan iguales
    }
}


// Transforma el texto para hacerlo todo en minusculas + quitar acentos y puntuaci�n
string normalize(const string& s) {
    string res;
    for (unsigned char c : s) {
        if (ispunct(c))                         // Si es signo de puntuaci�n lo reemplaza por espacio
            res += ' ';
        else
            res += foldAccent(tolower(c));     // Lo vuelve todo minuscula y quita acentos
    }
    return res;
}

// Establecemos stopwords, es decir palabras que no aportan significado en la pregunta del usuario y probablemente sean utilizadas
// Las establecemos para filtrarlas al realizar consultas
static const unordered_set<string> STOPWORDS = {
    "el","la","los","las","un","una","unos","unas",
    "y","o","de","del","en","para","por","con","cual","es"
};

// Dividimos la entrada de texto en palabras o tokens, tambi�n filtramos las stopwords
vector<string> tokenize(const string& s) {
    vector<string> tokens;
    istringstream iss(normalize(s));           // Usar texto normalizado
    string w;
    while (iss >> w) {                          // Lee palabra a palabra
        if (!STOPWORDS.count(w))                // Si no es stopword la a�ade al vector
            tokens.push_back(w);
    }
    return tokens;
}


// ---------------------- Consulta a Archivo de Texto ----------------------
// Se realiza un mapeo con clave (pais, campo) normalizado -> valor leido desde BD.txt
map<pair<string, string>, string> datosTXT;

// Carga archivo de texto con datos en formato "pais|campo|valor"
void cargarBDTxt(const string& archivo) {
    ifstream in(archivo);
    if (!in.is_open()) {                        // Verifica si la apertura fue exitosa
        cerr << "Error al abrir " << archivo << endl;
        return;
    }
    string linea;
    while (getline(in, linea)) {               // Lee cada l�nea
        stringstream ss(linea);
        string pais, campo, valor;
        if (getline(ss, pais, '|')             // Extrae pa�s
            && getline(ss, campo, '|')            // Extrae campo
            && getline(ss, valor)) {              // El resto es valor
            datosTXT[{normalize(pais), normalize(campo)}] = valor;
        }
    }
    in.close();                                 // Cierra archivo
}

// Enumera los posibles campos de consulta
enum class Campo { Capital, Poblacion, Territorio, DatoCurioso, Ninguno };

// Convierte Campo a cadena clave usada en mapas y SQL
string campoToString(Campo c) {
    switch (c) {
    case Campo::Capital:      return "capital";
    case Campo::Poblacion:    return "poblacion";
    case Campo::Territorio:   return "territorio";
    case Campo::DatoCurioso:  return "dato";
    default:                  return "";
    }
}

// Determina qu� campo pregunta el usuario, seg�n los tokens
Campo detectarCampo(const vector<string>& toks) {
    for (auto& t : toks) {
        if (t.find("territor") != string::npos) return Campo::Territorio;
        if (t.find("poblaci") != string::npos) return Campo::Poblacion;
        if (t.find("capital") != string::npos) return Campo::Capital;
        if (t == "dato" || t == "curioso")      return Campo::DatoCurioso;
    }
    return Campo::Ninguno;
}

// Extrae el pa�s de los tokens, omitiendo aquellos que describen el campo
string detectarPais(const vector<string>& toks, Campo campo) {
    for (auto& w : toks) {
        // Ignorar palabras clave de campo
        if (w == "territorio" || w.find("poblaci") != string::npos ||
            w == "capital" || w == "dato" ||
            w == "curioso")
            continue;
        return w;                                  // Primer token restante es pa�s
    }
    return "";                                   // No se detect� pa�s
}

// ---------------------- Consulta SQL Server ----------------------
string consultarBD(const string& pais, Campo campo) {
    // 1. Mapear el valor de 'campo' (enum) al nombre de la columna SQL correspondiente
    string col;
    if (campo == Campo::Capital)        col = "nombre_capital";   // buscamos la columna nombre_capital
    else if (campo == Campo::Poblacion) col = "poblacion";        // buscamos la columna poblacion
    else if (campo == Campo::Territorio)col = "territorio_km2";   // buscamos la columna territorio_km2
    else if (campo == Campo::DatoCurioso) col = "dato_curioso";    // buscamos la columna dato_curioso
    else
        return string();               // Si el campo no es v�lido, devolvemos cadena vac�a

    try {
        // 2. Crear y configurar la conexi�n ADO.NET a SQL Server
        //    - Data Source: servidor local SQLEXPRESS
        //    - Initial Catalog: base de datos CapitalesLatinoamerica
        //    - Integrated Security: autenticaci�n de Windows
        SqlConnection^ cn = gcnew SqlConnection(
            "Data Source=localhost\\SQLEXPRESS;"
            "Initial Catalog=CapitalesLatinoamerica;"
            "Integrated Security=True;"
        );
        cn->Open();  // Abrimos la conexi�n al servidor

        // 3. Construir la consulta parametrizada
        //    Usamos LOWER(...) para comparar sin distinguir may�sculas/min�sculas
        String^ sql = "SELECT " + gcnew String(col.c_str())
            + " FROM Capitales WHERE LOWER(pais)=LOWER(@pais)";
        SqlCommand^ cmd = gcnew SqlCommand(sql, cn);

        // 4. A�adir el par�metro @pais con el nombre del pa�s proporcionado
        //    Convertimos std::string a System::String^
        cmd->Parameters->AddWithValue("@pais", gcnew String(pais.c_str()));

        // 5. Ejecutar la consulta y obtener el lector de resultados
        SqlDataReader^ rd = cmd->ExecuteReader();
        string res;  // Variable para almacenar el resultado

        // 6. Leer la primera fila (si existe)
        if (rd->Read()) {
            // rd[0] es el valor de la columna solicitada;
            // marshal_as<string> convierte System::String^ a std::string
            res = marshal_as<string>(rd[0]->ToString());
        }

        // 7. Cerrar el lector y la conexi�n para liberar recursos
        rd->Close();
        cn->Close();

        // 8. Devolver el valor encontrado (o cadena vac�a si no hubo resultados)
        return res;
    }
    catch (Exception^ e) {
        // 9. Manejo de errores: capturamos cualquier excepci�n .NET
        //    e->Message contiene la descripci�n del fallo
        cerr << "Error SQL: " << marshal_as<string>(e->Message) << endl;
        return string();  // Retornamos cadena vac�a para indicar el fallo
    }
}

// ---------------------- cURL callback ----------------------
// Es una funci�n que libcurl llama para escribir datos recibidos en un string
// ---------------------- Callback para libcurl ----------------------
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    // Esta funci�n la invoca libcurl cada vez que recibe un fragmento de datos HTTP.
    // 'contents' apunta al b�fer con datos recibidos.
    // 'size' es el tama�o, en bytes, de cada unidad recibida.
    // 'nmemb' es el n�mero de esas unidades; el total de bytes = size * nmemb.
    // 'userp' es nuestro puntero de usuario: aqu� apuntamos a un std::string vac�o
    // que queremos rellenar con toda la respuesta.

    // Convertimos el void* a std::string* y a�adimos al final del string
    ((std::string*)userp)->append(
        (char*)contents,     // origen de los bytes (cast a char*)
        size * nmemb         // n�mero total de bytes que copiar
    );

    // Debemos devolver la cantidad de bytes procesados
    // Si devolvemos menos, libcurl interpretar� un error y abortar� la transferencia.
    return size * nmemb;
}


// ---------------------- Consulta OpenAI (ChatGPT) ----------------------
// Env�a prompt y devuelve la respuesta JSON cruda
string consultarChatGPT(const string& pregunta, const string& apiKey) {
    // ------------------ Construcci�n del payload JSON ------------------
    // Armamos manualmente el JSON que se enviar� a la API de OpenAI.
    // Incluye un mensaje "system" que define el rol del modelo
    // y un mensaje "user" que contiene la pregunta real.
    string data = "{\"model\":\"gpt-3.5-turbo\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"Eres un experto en geografia de Latinoamerica.\"},"
        "{\"role\":\"user\",\"content\":\"";

    // Escapamos comillas dobles y backslashes en la pregunta del usuario
    for (char c : pregunta) {
        if (c == '"' || c == '\\') data += '\\';  // escape manual de caracteres especiales
        data += c;
    }

    data += "\"}]}"; // Cerramos el JSON

    // ------------------ Inicializaci�n de cURL ------------------
    CURL* curl = curl_easy_init(); // Inicializa la sesi�n cURL
    string raw;                    // Aqu� se guardar� la respuesta JSON
    if (!curl) return "Error inicializando cURL."; // Si falla, se retorna error

    // ------------------ Configuraci�n de la solicitud ------------------
    struct curl_slist* hdrs = nullptr;

    // Agregamos encabezado de autorizaci�n con la API key
    hdrs = curl_slist_append(hdrs, (string("Authorization: Bearer ") + apiKey).c_str());

    // Indicamos que el contenido enviado es JSON
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    // Establecer URL de la API de OpenAI
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/chat/completions");

    // Establecer los headers construidos anteriormente
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    // Enviamos el cuerpo (payload) JSON
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());

    // Definimos c�mo se recibir� la respuesta: usando WriteCallback
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);

    // Indicamos d�nde guardar la respuesta que devuelve el servidor
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &raw);

    // ------------------ Enviar solicitud ------------------
    curl_easy_perform(curl); // Ejecuta la solicitud HTTP

    // ------------------ Limpieza ------------------
    curl_slist_free_all(hdrs); // Liberamos memoria de headers
    curl_easy_cleanup(curl);  // Cerramos sesi�n cURL

    // ------------------ Devolver respuesta ------------------
    return raw; // Devolvemos la respuesta JSON completa como string
}

// ---------------------- Funci�n principal ----------------------
int main() {
    cargarBDTxt("BD.txt");  // 1) Carga en memoria todos los datos previos desde BD.txt


    // 2) Mostrar banner y confirmar que todas las fuentes de datos est�n listas
    cout << GRAY << "Programacion I\nGrupo 6\nCristopher Hwang\nKenneth Morales\nEmerson Reyes\nGerson Monte Negro\n" << RESET << endl;
    cout << GRAY << "Conexion BD local, archivo de texto y API ChatGPT exitosa.\n" << RESET << endl;

    // 3) Instrucci�n b�sica al usuario
    cout << "Chatbot sobre paises de Latinoamerica. Escribe 'salir' para terminar." << endl;


    while (true) {
        cout << "\nTu: ";
        string pregunta;
        getline(cin, pregunta);             // 4) Leer toda la l�nea que ingresa el usuario
        if (normalize(pregunta) == "salir") // 5) Si, tras normalizar (min�sculas, sin tildes, sin puntuaci�n), coincide con "salir"
            break;                          //    salimos del bucle y terminamos el programa


        auto toks = tokenize(pregunta);          // 6) Normaliza, quita tildes/puntuaci�n y divide en tokens �tiles
        Campo campo = detectarCampo(toks);       // 7) Busca entre los tokens cu�l es la intenci�n (capital, poblaci�n...)
        string pais = detectarPais(toks, campo); // 8) Extrae el pa�s de entre los tokens
        if (campo == Campo::Ninguno || pais.empty()) {
            cout << "Bot: No entendi tu pregunta." << endl;
            continue;                            // 9) Si no detecta ni campo ni pa�s, pide otra pregunta
        }


        // Intento 1: buscar en archivo de texto
        cout << RED << "(Archivo) Analizando pregunta..." << RESET << endl;
        string respuesta = datosTXT[{pais, campoToString(campo)}]; // 10) Busca la clave (pa�s, campo) en el mapa
        if (!respuesta.empty()) {
            if (campo == Campo::Territorio) respuesta += " km";
            cout << RED << "Bot (Archivo): " << campoToString(campo) << ": " << respuesta << RESET << endl << endl;
            continue;                          // 11) Si encontr� respuesta, la muestra y vuelve al inicio del bucle
        }


        // Intento 2: buscar en SQL
        cout << "Bot: No encontre la respuesta en el archivo. Deseas buscar en la base de datos? (si/no): ";
        string decision;
        getline(cin, decision);
        if (normalize(decision) == "si") {
            cout << GRAY << "(BD) Analizando pregunta..." << RESET << endl;
            respuesta = consultarBD(pais, campo); // 12) Llama a la funci�n que hace el query a SQL Server
            if (!respuesta.empty()) {
                if (campo == Campo::Territorio) respuesta += " km";
                cout << GRAY << "Bot (BD): " << campoToString(campo) << " de " << pais << ": " << respuesta << RESET << endl << endl;
            }
            else {
                cout << "Bot: Tampoco encontre respuesta en la base de datos." << endl;
                // 13) Si tampoco en BD, pasar al siguiente paso: ChatGPT

                // Intento 3: ChatGPT
                cout << GREEN << "(ChatGPT) Analizando pregunta..." << RESET << endl;
                cout << "Bot: �Quieres que pregunte a ChatGPT? (si/no): ";
                getline(cin, decision);
                if (normalize(decision) == "si") {
                    string apiKey = "TU_OPENAI_API_KEY";
                    string gptRaw = consultarChatGPT(pregunta, apiKey); // 14) Llamada HTTP a la API de OpenAI

                    // 15) Extraer el campo "content" del JSON recibido
                    size_t pos = gptRaw.find("\"content\"");
                    if (pos != string::npos) {
                        size_t start = gptRaw.find('"', pos + 10) + 1;
                        size_t end = gptRaw.find('"', start);
                        string content = gptRaw.substr(start, end - start);

                        // 16) Limpiar acentos y caracteres no ASCII para consola
                        string ascii;
                        for (unsigned char c : content) {
                            if (c < 128 && isprint(c)) {
                                switch (c) {
                                case '�': ascii += 'a'; break;
                                case '�': ascii += 'e'; break;
                                case '�': ascii += 'i'; break;
                                case '�': ascii += 'o'; break;
                                case '�': ascii += 'u'; break;
                                default: ascii += c;
                                }
                            }
                        }
                        cout << GREEN << "Bot (ChatGPT): " << ascii << RESET << endl << endl;
                    }
                    else {
                        cout << GREEN << "Bot (ChatGPT): " << gptRaw << RESET << endl << endl;
                    }
                }
            }
        else {
            cout << "Bot: Entendido, no consultare en la base de datos." << endl;
        }


        cout << "Bot: Hasta luego" << endl;  // 17) Mensaje de despedida
        return 0;                            // 18) C�digo de salida 0 = �xito
        }