/* ------------------------------------------------------------
 * UNIVERSIDAD DEL VALLE DE GUATEMALA
 * FACULTAD DE INGENIERIA
 * DEPARTAMENTO DE CIENCIA DE LA COMPUTACION
 *
 * Curso: CC3086 Programacion de Microprocesadores
 * Ejemplo: Compresion secuencial de un archivo
 * ------------------------------------------------------------
 * Descripcion:
 * Este programa toma un archivo de entrada y genera una
 * version comprimida usando la libreria zlib.
 * ------------------------------------------------------------
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <zlib.h>

using namespace std;

int main(int argc, char* argv[]) {
    if (argc < 3) {
        cerr << "Uso: " << argv[0] << " <archivo_entrada> <archivo_salida>" << endl;
        return 1;
    }

    const char* inputFile = argv[1];
    const char* outputFile = argv[2];

    // Leer todo el archivo de entrada en memoria
    ifstream in(inputFile, ios::binary | ios::ate);
    if (!in) {
        cerr << "No se pudo abrir el archivo de entrada." << endl;
        return 1;
    }

    streamsize size = in.tellg();
    in.seekg(0, ios::beg);

    vector<char> buffer(size);
    if (!in.read(buffer.data(), size)) {
        cerr << "Error al leer el archivo." << endl;
        return 1;
    }
    in.close();

    // Calcular el tamaño máximo necesario para la compresión
    uLongf compressedSize = compressBound(size);
    vector<Bytef> compressedData(compressedSize);

    // Comprimir
    int res = compress(compressedData.data(), &compressedSize,
                       reinterpret_cast<const Bytef*>(buffer.data()), size);

    if (res != Z_OK) {
        cerr << "Error al comprimir (codigo " << res << ")" << endl;
        return 1;
    }

    // Guardar en el archivo de salida
    ofstream out(outputFile, ios::binary);
    if (!out) {
        cerr << "No se pudo abrir el archivo de salida." << endl;
        return 1;
    }

    out.write(reinterpret_cast<char*>(compressedData.data()), compressedSize);
    out.close();

    cout << "Archivo comprimido exitosamente: " << outputFile << endl;
    cout << "Tamano original: " << size << " bytes" << endl;
    cout << "Tamano comprimido: " << compressedSize << " bytes" << endl;

    return 0;
}
