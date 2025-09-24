/* ------------------------------------------------------------
 * UNIVERSIDAD DEL VALLE DE GUATEMALA
 * FACULTAD DE INGENIERIA  
 * DEPARTAMENTO DE CIENCIA DE LA COMPUTACION
 * AUTOR: DENIL JOSÈ PARADA CABRERA - 24761
 * Curso: CC3086 Programacion de Microprocesadores
 * Laboratorio 7: Compresion paralela de archivos
 * ------------------------------------------------------------
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <pthread.h>
#include <zlib.h>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>
#include <iomanip>

using namespace std;
using namespace chrono;

// Estructura para metadata de bloques
struct BlockInfo {
    size_t originalSize;
    size_t compressedSize;
};

// Estructura para pasar datos a los hilos de compresión
struct ThreadData {
    int threadId;
    vector<char>* inputData;
    size_t startPos;
    size_t blockSize;
    vector<char> compressedData;
    size_t compressedSize;
    bool compressionSuccess;
};

// Estructura para pasar datos a los hilos de descompresión
struct DecompressThreadData {
    int threadId;
    vector<char>* compressedData;
    size_t startPos;
    size_t compressedSize;
    vector<char> decompressedData;
    size_t originalSize;
    bool decompressionSuccess;
};

// Variables globales para sincronización
pthread_mutex_t writeMutex = PTHREAD_MUTEX_INITIALIZER;

// Función para verificar si dos archivos son idénticos
bool verifyFiles(const string& file1, const string& file2) {
    ifstream f1(file1, ios::binary);
    ifstream f2(file2, ios::binary);
    
    if (!f1 || !f2) {
        cout << "Error: No se pudieron abrir los archivos para verificación." << endl;
        return false;
    }
    
    // Comparar tamaños primero
    f1.seekg(0, ios::end);
    f2.seekg(0, ios::end);
    
    if (f1.tellg() != f2.tellg()) {
        cout << "Los archivos tienen tamaños diferentes." << endl;
        return false;
    }
    
    // Comparar contenido byte a byte
    f1.seekg(0, ios::beg);
    f2.seekg(0, ios::beg);
    
    return equal(istreambuf_iterator<char>(f1),
                 istreambuf_iterator<char>(),
                 istreambuf_iterator<char>(f2));
}

// Función para comprimir un bloque
void* compressBlock(void* arg) {
    ThreadData* data = static_cast<ThreadData*>(arg);
    
    // Calcular el tamaño real del bloque
    size_t actualBlockSize = min(data->blockSize, data->inputData->size() - data->startPos);
    
    // Calcular el tamaño máximo necesario para la compresión
    uLongf maxCompressedSize = compressBound(actualBlockSize);
    data->compressedData.resize(maxCompressedSize);
    
    // Comprimir el bloque
    uLongf compressedSize = maxCompressedSize;
    int result = compress(
        reinterpret_cast<Bytef*>(data->compressedData.data()),
        &compressedSize,
        reinterpret_cast<const Bytef*>(data->inputData->data() + data->startPos),
        actualBlockSize
    );
    
    if (result == Z_OK) {
        data->compressedData.resize(compressedSize);
        data->compressedSize = compressedSize;
        data->compressionSuccess = true;
        
        cout << "Hilo " << data->threadId << " comprimió bloque de " 
             << actualBlockSize << " bytes a " << compressedSize << " bytes" << endl;
    } else {
        data->compressionSuccess = false;
        cerr << "Error en compresión del hilo " << data->threadId << endl;
    }
    
    return nullptr;
}

// Función para descomprimir un bloque
void* decompressBlock(void* arg) {
    DecompressThreadData* data = static_cast<DecompressThreadData*>(arg);
    
    // Redimensionar el buffer de salida al tamaño original conocido
    data->decompressedData.resize(data->originalSize);
    
    uLongf decompressedSize = data->originalSize;
    int result = uncompress(
        reinterpret_cast<Bytef*>(data->decompressedData.data()),
        &decompressedSize,
        reinterpret_cast<const Bytef*>(data->compressedData->data() + data->startPos),
        data->compressedSize
    );
    
    if (result == Z_OK) {
        data->decompressionSuccess = true;
        cout << "Hilo " << data->threadId << " descomprimió bloque de " 
             << data->compressedSize << " bytes a " << decompressedSize << " bytes" << endl;
    } else {
        data->decompressionSuccess = false;
        cerr << "Error en descompresión del hilo " << data->threadId << " (código " << result << ")" << endl;
    }
    
    return nullptr;
}

int compressFile(const string& inputFile, const string& outputFile, int numThreads) {
    cout << "\n=== COMPRESIÓN PARALELA ===" << endl;
    cout << "Archivo de entrada: " << inputFile << endl;
    cout << "Archivo de salida: " << outputFile << endl;
    cout << "Número de hilos: " << numThreads << endl;
    
    auto start = high_resolution_clock::now();
    
    // Leer el archivo completo
    ifstream input(inputFile, ios::binary | ios::ate);
    if (!input) {
        cerr << "Error: No se pudo abrir el archivo de entrada." << endl;
        return 1;
    }
    
    size_t fileSize = input.tellg();
    input.seekg(0, ios::beg);
    
    vector<char> inputData(fileSize);
    if (!input.read(inputData.data(), fileSize)) {
        cerr << "Error al leer el archivo." << endl;
        return 1;
    }
    input.close();
    
    cout << "Tamaño del archivo original: " << fileSize << " bytes" << endl;
    
    // Calcular el tamaño de bloque (1 MB por defecto)
    size_t blockSize = 1024 * 1024; // 1 MB
    int numBlocks = (fileSize + blockSize - 1) / blockSize;
    
    cout << "Dividiendo en " << numBlocks << " bloques de ~" << (blockSize / 1024) << " KB cada uno" << endl;
    
    // Crear estructuras de datos
    vector<ThreadData> threadData(numBlocks);
    vector<pthread_t> threads(numThreads);
    vector<BlockInfo> blockInfo(numBlocks);
    
    // Procesar bloques en lotes
    for (int startBlock = 0; startBlock < numBlocks; startBlock += numThreads) {
        int blocksInThisBatch = min(numThreads, numBlocks - startBlock);
        
        // Configurar datos para cada hilo
        for (int i = 0; i < blocksInThisBatch; i++) {
            int blockId = startBlock + i;
            threadData[blockId].threadId = blockId;
            threadData[blockId].inputData = &inputData;
            threadData[blockId].startPos = blockId * blockSize;
            threadData[blockId].blockSize = blockSize;
        }
        
        // Crear hilos
        for (int i = 0; i < blocksInThisBatch; i++) {
            int blockId = startBlock + i;
            if (pthread_create(&threads[i], nullptr, compressBlock, &threadData[blockId]) != 0) {
                cerr << "Error al crear hilo " << i << endl;
                return 1;
            }
        }
        
        // Esperar a que terminen todos los hilos
        for (int i = 0; i < blocksInThisBatch; i++) {
            pthread_join(threads[i], nullptr);
        }
    }
    
    // Escribir archivo comprimido con metadata
    ofstream output(outputFile, ios::binary);
    if (!output) {
        cerr << "Error: No se pudo abrir el archivo de salida." << endl;
        return 1;
    }
    
    // Escribir header: número de bloques
    output.write(reinterpret_cast<const char*>(&numBlocks), sizeof(int));
    
    // Calcular metadata de bloques
    size_t totalCompressed = 0;
    for (int i = 0; i < numBlocks; i++) {
        if (threadData[i].compressionSuccess) {
            size_t actualOriginalSize = min(blockSize, fileSize - i * blockSize);
            blockInfo[i].originalSize = actualOriginalSize;
            blockInfo[i].compressedSize = threadData[i].compressedSize;
            totalCompressed += threadData[i].compressedSize;
        } else {
            cerr << "Error en la compresión del bloque " << i << endl;
            return 1;
        }
    }
    
    // Escribir metadata de todos los bloques
    output.write(reinterpret_cast<const char*>(blockInfo.data()), 
                numBlocks * sizeof(BlockInfo));
    
    // Escribir datos comprimidos en orden
    for (int i = 0; i < numBlocks; i++) {
        output.write(threadData[i].compressedData.data(), threadData[i].compressedSize);
    }
    
    output.close();
    
    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start);
    
    cout << "\n=== RESULTADOS ===" << endl;
    cout << "Compresión completada exitosamente" << endl;
    cout << "Tamaño original: " << fileSize << " bytes" << endl;
    cout << "Tamaño comprimido: " << totalCompressed << " bytes" << endl;
    cout << "Metadata: " << (sizeof(int) + numBlocks * sizeof(BlockInfo)) << " bytes" << endl;
    cout << "Archivo total: " << (totalCompressed + sizeof(int) + numBlocks * sizeof(BlockInfo)) << " bytes" << endl;
    cout << "Ratio de compresión: " << fixed << setprecision(2) << (100.0 - (totalCompressed * 100.0 / fileSize)) << "%" << endl;
    cout << "Tiempo de ejecución: " << duration.count() << " ms" << endl;
    
    return 0;
}

int decompressFile(const string& inputFile, const string& outputFile, int numThreads) {
    cout << "\n=== DESCOMPRESIÓN PARALELA ===" << endl;
    cout << "Archivo comprimido: " << inputFile << endl;
    cout << "Archivo de salida: " << outputFile << endl;
    cout << "Número de hilos: " << numThreads << endl;
    
    auto start = high_resolution_clock::now();
    
    // Leer archivo comprimido
    ifstream input(inputFile, ios::binary);
    if (!input) {
        cerr << "Error: No se pudo abrir el archivo comprimido." << endl;
        return 1;
    }
    
    // Leer número de bloques
    int numBlocks;
    input.read(reinterpret_cast<char*>(&numBlocks), sizeof(int));
    
    cout << "Número de bloques: " << numBlocks << endl;
    
    // Leer metadata de bloques
    vector<BlockInfo> blockInfo(numBlocks);
    input.read(reinterpret_cast<char*>(blockInfo.data()), 
              numBlocks * sizeof(BlockInfo));
    
    // Calcular tamaño total de datos comprimidos
    size_t totalCompressedSize = 0;
    size_t totalOriginalSize = 0;
    for (int i = 0; i < numBlocks; i++) {
        totalCompressedSize += blockInfo[i].compressedSize;
        totalOriginalSize += blockInfo[i].originalSize;
        cout << "Bloque " << i << ": " << blockInfo[i].originalSize 
             << " -> " << blockInfo[i].compressedSize << " bytes" << endl;
    }
    
    // Leer todos los datos comprimidos
    vector<char> allCompressedData(totalCompressedSize);
    input.read(allCompressedData.data(), totalCompressedSize);
    input.close();
    
    cout << "Datos comprimidos leídos: " << totalCompressedSize << " bytes" << endl;
    cout << "Tamaño original esperado: " << totalOriginalSize << " bytes" << endl;
    
    // Preparar estructuras para descompresión
    vector<DecompressThreadData> threadData(numBlocks);
    vector<pthread_t> threads(numThreads);
    
    // Procesar bloques en lotes
    size_t currentPos = 0;
    for (int startBlock = 0; startBlock < numBlocks; startBlock += numThreads) {
        int blocksInThisBatch = min(numThreads, numBlocks - startBlock);
        
        // Configurar datos para cada hilo
        for (int i = 0; i < blocksInThisBatch; i++) {
            int blockId = startBlock + i;
            threadData[blockId].threadId = blockId;
            threadData[blockId].compressedData = &allCompressedData;
            threadData[blockId].startPos = currentPos;
            threadData[blockId].compressedSize = blockInfo[blockId].compressedSize;
            threadData[blockId].originalSize = blockInfo[blockId].originalSize;
            
            currentPos += blockInfo[blockId].compressedSize;
        }
        
        // Crear hilos
        for (int i = 0; i < blocksInThisBatch; i++) {
            int blockId = startBlock + i;
            if (pthread_create(&threads[i], nullptr, decompressBlock, &threadData[blockId]) != 0) {
                cerr << "Error al crear hilo " << i << endl;
                return 1;
            }
        }
        
        // Esperar a que terminen todos los hilos
        for (int i = 0; i < blocksInThisBatch; i++) {
            pthread_join(threads[i], nullptr);
        }
        
        // Reiniciar posición para el siguiente lote
        currentPos = 0;
        for (int j = 0; j <= startBlock + blocksInThisBatch - 1; j++) {
            currentPos += blockInfo[j].compressedSize;
        }
    }
    
    // Escribir archivo descomprimido
    ofstream output(outputFile, ios::binary);
    if (!output) {
        cerr << "Error: No se pudo abrir el archivo de salida." << endl;
        return 1;
    }
    
    size_t totalWritten = 0;
    for (int i = 0; i < numBlocks; i++) {
        if (threadData[i].decompressionSuccess) {
            output.write(threadData[i].decompressedData.data(), blockInfo[i].originalSize);
            totalWritten += blockInfo[i].originalSize;
        } else {
            cerr << "Error en la descompresión del bloque " << i << endl;
            return 1;
        }
    }
    
    output.close();
    
    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start);
    
    cout << "\n=== RESULTADOS ===" << endl;
    cout << "Descompresión completada exitosamente" << endl;
    cout << "Archivo guardado como: " << outputFile << endl;
    cout << "Tamaño descomprimido: " << totalWritten << " bytes" << endl;
    cout << "Tiempo de ejecución: " << duration.count() << " ms" << endl;
    
    return 0;
}

// Función para ejecutar benchmark con diferentes números de hilos
void runBenchmark() {
    cout << "\n=== BENCHMARK AUTOMÁTICO ===" << endl;
    cout << "Probando compresión con diferentes números de hilos..." << endl;
    
    vector<int> threadCounts = {1, 2, 4, 8, 16, 32};
    string inputFile = "paralelismo_teoria.txt";
    
    // Verificar que existe el archivo
    ifstream test(inputFile);
    if (!test.good()) {
        cout << "Error: No se encontró el archivo " << inputFile << endl;
        cout << "Asegúrate de que el archivo esté en el directorio actual." << endl;
        return;
    }
    test.close();
    
    cout << "\n" << setw(8) << "Hilos" << setw(15) << "Tiempo(ms)" << setw(20) << "Speedup" << endl;
    cout << "--------" << "---------------" << "--------------------" << endl;
    
    double baselineTime = 0;
    
    for (size_t i = 0; i < threadCounts.size(); i++) {
        int threads = threadCounts[i];
        string outputFile = "benchmark_" + to_string(threads) + "_hilos.bin";
        
        cout << "\nProbando con " << threads << " hilo(s)..." << endl;
        
        auto start = high_resolution_clock::now();
        int result = compressFile(inputFile, outputFile, threads);
        auto end = high_resolution_clock::now();
        
        if (result != 0) {
            cout << "Error en la compresión con " << threads << " hilos" << endl;
            continue;
        }
        
        auto duration = duration_cast<milliseconds>(end - start);
        double timeMs = duration.count();
        
        if (i == 0) {
            baselineTime = timeMs;
        }
        
        double speedup = (baselineTime > 0) ? baselineTime / timeMs : 1.0;
        
        cout << setw(8) << threads << setw(15) << fixed << setprecision(0) << timeMs 
             << setw(20) << setprecision(2) << speedup << "x" << endl;
        
        // Limpiar archivo temporal
        remove(outputFile.c_str());
    }
    
    cout << "\n=== ANÁLISIS ===" << endl;
    cout << "• Baseline (1 hilo): " << baselineTime << " ms" << endl;
    cout << "• El speedup muestra cuántas veces más rápido es vs. 1 hilo" << endl;
    cout << "• Valores > 1.0x indican mejora de rendimiento" << endl;
    cout << "• Si el speedup decrece, indica saturación o overhead" << endl;
}

// Función para probar diferentes tamaños de bloque
void testBlockSizes() {
    cout << "\n=== PRUEBA DE TAMAÑOS DE BLOQUE ===" << endl;
    cout << "Probando diferentes tamaños de bloque con 4 hilos..." << endl;
    
    string inputFile = "paralelismo_teoria.txt";
    
    // Verificar que existe el archivo
    ifstream test(inputFile);
    if (!test.good()) {
        cout << "Error: No se encontró el archivo " << inputFile << endl;
        return;
    }
    test.close();
    
    // Esta función requeriría modificar compressFile para aceptar blockSize como parámetro
    cout << "Nota: Para implementar esta función completamente, se necesita modificar" << endl;
    cout << "compressFile() para aceptar blockSize como parámetro." << endl;
    cout << "Tamaño actual: 1 MB (1024*1024 bytes)" << endl;
}

void showMenu() {
    cout << "\n========================================" << endl;
    cout << "    COMPRESIÓN PARALELA DE ARCHIVOS" << endl;
    cout << "========================================" << endl;
    cout << "1. Comprimir archivo" << endl;
    cout << "2. Descomprimir archivo" << endl;
    cout << "3. Verificar integridad de archivos" << endl;
    cout << "4. Ejecutar benchmark de rendimiento" << endl;
    cout << "5. Probar tamaños de bloque" << endl;
    cout << "6. Salir" << endl;
    cout << "========================================" << endl;
    cout << "Selecciona una opción: ";
}

int main() {
    int option;
    string inputFile, outputFile;
    int numThreads;
    
    cout << "=== LABORATORIO 7: COMPRESIÓN PARALELA ===" << endl;
    cout << "Autor: DENIL JOSÉ PARADA CABRERA - 24761" << endl;
    
    while (true) {
        showMenu();
        cin >> option;
        
        switch (option) {
            case 1: {
                cout << "\n--- COMPRESIÓN ---" << endl;
                cout << "Archivo de entrada: ";
                cin >> inputFile;
                cout << "Archivo de salida: ";
                cin >> outputFile;
                cout << "Número de hilos: ";
                cin >> numThreads;
                
                if (numThreads <= 0) {
                    cout << "Número de hilos debe ser positivo. Usando 1 hilo." << endl;
                    numThreads = 1;
                }
                
                compressFile(inputFile, outputFile, numThreads);
                break;
            }
            
            case 2: {
                cout << "\n--- DESCOMPRESIÓN ---" << endl;
                cout << "Archivo comprimido: ";
                cin >> inputFile;
                cout << "Archivo de salida: ";
                cin >> outputFile;
                cout << "Número de hilos: ";
                cin >> numThreads;
                
                if (numThreads <= 0) {
                    cout << "Número de hilos debe ser positivo. Usando 1 hilo." << endl;
                    numThreads = 1;
                }
                
                decompressFile(inputFile, outputFile, numThreads);
                break;
            }
            
            case 3: {
                cout << "\n--- VERIFICACIÓN DE INTEGRIDAD ---" << endl;
                string file1, file2;
                cout << "Archivo original: ";
                cin >> file1;
                cout << "Archivo descomprimido: ";
                cin >> file2;
                
                cout << "Verificando integridad..." << endl;
                if (verifyFiles(file1, file2)) {
                    cout << "✓ Los archivos son IDÉNTICOS" << endl;
                    cout << "La compresión/descompresión fue exitosa." << endl;
                } else {
                    cout << "✗ Los archivos son DIFERENTES" << endl;
                    cout << "Hubo un problema en el proceso." << endl;
                }
                break;
            }
            
            case 4: {
                runBenchmark();
                break;
            }
            
            case 5: {
                testBlockSizes();
                break;
            }
            
            case 6:
                cout << "\n¡Gracias por usar el programa!" << endl;
                cout << "Laboratorio completado exitosamente." << endl;
                return 0;
                
            default:
                cout << "Opción inválida. Por favor, selecciona 1-6." << endl;
                break;
        }
        
        cout << "\nPresiona Enter para continuar...";
        cin.ignore();
        cin.get();
    }
    
    return 0;
}