/* ------------------------------------------------------------
 * UNIVERSIDAD DEL VALLE DE GUATEMALA
 * FACULTAD DE INGENIERIA
 * DEPARTAMENTO DE CIENCIA DE LA COMPUTACION
 *
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

using namespace std;
using namespace chrono;

// Estructura para pasar datos a los hilos
struct ThreadData {
    int threadId;
    vector<char>* inputData;
    size_t startPos;
    size_t blockSize;
    vector<char>* compressedData;
    size_t* compressedSize;
    bool compressionSuccess;
};

struct DecompressThreadData {
    int threadId;
    vector<char>* compressedData;
    size_t startPos;
    size_t blockSize;
    vector<char>* decompressedData;
    size_t* decompressedSize;
    bool decompressionSuccess;
};

// Variables globales para sincronización
pthread_mutex_t writeMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t orderMutex = PTHREAD_MUTEX_INITIALIZER;
vector<bool> blockCompleted;
int nextBlockToWrite = 0;

// Función para comprimir un bloque
void* compressBlock(void* arg) {
    ThreadData* data = static_cast<ThreadData*>(arg);
    
    // Calcular el tamaño real del bloque (puede ser menor en el último bloque)
    size_t actualBlockSize = min(data->blockSize, data->inputData->size() - data->startPos);
    
    // Calcular el tamaño máximo necesario para la compresión
    uLongf maxCompressedSize = compressBound(actualBlockSize);
    data->compressedData->resize(maxCompressedSize);
    
    // Comprimir el bloque
    uLongf compressedSize = maxCompressedSize;
    int result = compress(
        reinterpret_cast<Bytef*>(data->compressedData->data()),
        &compressedSize,
        reinterpret_cast<const Bytef*>(data->inputData->data() + data->startPos),
        actualBlockSize
    );
    
    if (result == Z_OK) {
        data->compressedData->resize(compressedSize);
        *(data->compressedSize) = compressedSize;
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
    
    // Para la descompresión, necesitamos conocer el tamaño original
    // En un caso real, esto se almacenaría en el archivo comprimido
    uLongf decompressedSize = data->blockSize * 4; // Estimación
    data->decompressedData->resize(decompressedSize);
    
    int result = uncompress(
        reinterpret_cast<Bytef*>(data->decompressedData->data()),
        &decompressedSize,
        reinterpret_cast<const Bytef*>(data->compressedData->data() + data->startPos),
        data->blockSize
    );
    
    if (result == Z_OK) {
        data->decompressedData->resize(decompressedSize);
        *(data->decompressedSize) = decompressedSize;
        data->decompressionSuccess = true;
        
        cout << "Hilo " << data->threadId << " descomprimió bloque de " 
             << data->blockSize << " bytes a " << decompressedSize << " bytes" << endl;
    } else {
        data->decompressionSuccess = false;
        cerr << "Error en descompresión del hilo " << data->threadId << endl;
    }
    
    return nullptr;
}

// Función para escribir los bloques en orden
void writeBlockInOrder(ofstream& output, vector<ThreadData>& threads, int blockId) {
    pthread_mutex_lock(&orderMutex);
    
    // Esperar hasta que sea el turno de este bloque
    while (nextBlockToWrite != blockId) {
        pthread_mutex_unlock(&orderMutex);
        usleep(1000); // Esperar 1ms
        pthread_mutex_lock(&orderMutex);
    }
    
    // Escribir el bloque
    pthread_mutex_lock(&writeMutex);
    output.write(threads[blockId].compressedData->data(), *threads[blockId].compressedSize);
    pthread_mutex_unlock(&writeMutex);
    
    nextBlockToWrite++;
    blockCompleted[blockId] = true;
    
    pthread_mutex_unlock(&orderMutex);
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
    
    // Ajustar número de hilos si hay menos bloques
    numThreads = min(numThreads, numBlocks);
    
    // Inicializar vectores de control
    blockCompleted.assign(numBlocks, false);
    nextBlockToWrite = 0;
    
    // Crear estructuras de datos para hilos
    vector<ThreadData> threadData(numBlocks);
    vector<pthread_t> threads(numThreads);
    vector<vector<char>> compressedBlocks(numBlocks);
    vector<size_t> compressedSizes(numBlocks);
    
    // Abrir archivo de salida
    ofstream output(outputFile, ios::binary);
    if (!output) {
        cerr << "Error: No se pudo abrir el archivo de salida." << endl;
        return 1;
    }
    
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
            threadData[blockId].compressedData = &compressedBlocks[blockId];
            threadData[blockId].compressedSize = &compressedSizes[blockId];
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
        
        // Escribir bloques en orden
        for (int i = 0; i < blocksInThisBatch; i++) {
            int blockId = startBlock + i;
            if (threadData[blockId].compressionSuccess) {
                output.write(compressedBlocks[blockId].data(), compressedSizes[blockId]);
            } else {
                cerr << "Error en la compresión del bloque " << blockId << endl;
                return 1;
            }
        }
    }
    
    output.close();
    
    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start);
    
    // Calcular tamaño total comprimido
    size_t totalCompressed = 0;
    for (size_t size : compressedSizes) {
        totalCompressed += size;
    }
    
    cout << "\n=== RESULTADOS ===" << endl;
    cout << "Compresión completada exitosamente" << endl;
    cout << "Tamaño original: " << fileSize << " bytes" << endl;
    cout << "Tamaño comprimido: " << totalCompressed << " bytes" << endl;
    cout << "Ratio de compresión: " << (100.0 - (totalCompressed * 100.0 / fileSize)) << "%" << endl;
    cout << "Tiempo de ejecución: " << duration.count() << " ms" << endl;
    
    return 0;
}

int decompressFile(const string& inputFile, const string& outputFile, int numThreads) {
    cout << "\n=== DESCOMPRESIÓN PARALELA ===" << endl;
    cout << "Esta implementación básica descomprime secuencialmente" << endl;
    cout << "Para descompresión paralela real, se necesita metadata adicional" << endl;
    
    auto start = high_resolution_clock::now();
    
    // Leer archivo comprimido
    ifstream input(inputFile, ios::binary | ios::ate);
    if (!input) {
        cerr << "Error: No se pudo abrir el archivo comprimido." << endl;
        return 1;
    }
    
    size_t compressedSize = input.tellg();
    input.seekg(0, ios::beg);
    
    vector<char> compressedData(compressedSize);
    if (!input.read(compressedData.data(), compressedSize)) {
        cerr << "Error al leer el archivo comprimido." << endl;
        return 1;
    }
    input.close();
    
    // Estimar tamaño descomprimido (en caso real, esto se almacenaría en el archivo)
    uLongf decompressedSize = compressedSize * 4;
    vector<char> decompressedData(decompressedSize);
    
    // Descomprimir
    int result = uncompress(
        reinterpret_cast<Bytef*>(decompressedData.data()),
        &decompressedSize,
        reinterpret_cast<const Bytef*>(compressedData.data()),
        compressedSize
    );
    
    if (result != Z_OK) {
        cerr << "Error en la descompresión (código " << result << ")" << endl;
        return 1;
    }
    
    // Ajustar tamaño y guardar
    decompressedData.resize(decompressedSize);
    
    ofstream output(outputFile, ios::binary);
    if (!output) {
        cerr << "Error: No se pudo abrir el archivo de salida." << endl;
        return 1;
    }
    
    output.write(decompressedData.data(), decompressedSize);
    output.close();
    
    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start);
    
    cout << "Descompresión completada exitosamente" << endl;
    cout << "Tamaño descomprimido: " << decompressedSize << " bytes" << endl;
    cout << "Tiempo de ejecución: " << duration.count() << " ms" << endl;
    
    return 0;
}

void showMenu() {
    cout << "\n========================================" << endl;
    cout << "    COMPRESIÓN PARALELA DE ARCHIVOS" << endl;
    cout << "========================================" << endl;
    cout << "1. Comprimir archivo" << endl;
    cout << "2. Descomprimir archivo" << endl;
    cout << "3. Salir" << endl;
    cout << "========================================" << endl;
    cout << "Selecciona una opción: ";
}

int main() {
    int option;
    string inputFile, outputFile;
    int numThreads;
    
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
                
                decompressFile(inputFile, outputFile, numThreads);
                break;
            }
            
            case 3:
                cout << "¡Hasta luego!" << endl;
                return 0;
                
            default:
                cout << "Opción inválida. Por favor, selecciona 1, 2 o 3." << endl;
                break;
        }
        
        cout << "\nPresiona Enter para continuar...";
        cin.ignore();
        cin.get();
    }
    
    return 0;
}