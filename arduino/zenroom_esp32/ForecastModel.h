#pragma once
// Aviso anticipado de calor: red 5-16-16-1 (385 parametros) entrenada en Colab. Misma inferencia que
// src/main.cpp de zenroom-embedded-app; los pesos estan en forecast_weights.h.
// Una "muestra" es un minuto de la sala (o MODEL_STEP_MS en simulacion). Con 11 muestras de historia
// la red estima si la sala ocupada llegara a su limite de temperatura en los proximos 15 minutos.
#include <math.h>
#include <string.h>
#include "forecast_weights.h"

class ForecastModel {
 public:
  float limitC = 25.0f;           // lo cambia el comando MQTT temp_limit
  float probability = 0.0f;
  bool historyReady = false;

  float infer(const float *raw) const {
    float x[NN_IN], h1[NN_H], h2[NN_H];
    for (int i = 0; i < NN_IN; ++i) x[i] = (raw[i] - NN_MU[i]) / NN_SD[i];
    for (int j = 0; j < NN_H; ++j) {
      float v = NN_B1[j];
      for (int i = 0; i < NN_IN; ++i) v += NN_W1[j * NN_IN + i] * x[i];
      h1[j] = v > 0 ? v : 0;
    }
    for (int j = 0; j < NN_H; ++j) {
      float v = NN_B2[j];
      for (int i = 0; i < NN_H; ++i) v += NN_W2[j * NN_H + i] * h1[i];
      h2[j] = v > 0 ? v : 0;
    }
    float y = NN_B3[0];
    for (int i = 0; i < NN_H; ++i) y += NN_W3[i] * h2[i];
    return 1.0f / (1.0f + expf(-y));
  }

  // Error maximo contra los 8 casos exportados de Keras; al arrancar debe ser < 1e-4.
  float selfTest() const {
    float worst = 0;
    for (int k = 0; k < REF_N; ++k) {
      float e = fabsf(infer(&REF_X[k * NN_IN]) - REF_P[k]);
      if (e > worst) worst = e;
    }
    return worst;
  }

  // Una muestra: temperatura, humedad, ocupacion (0/1) y Leq del periodo. Devuelve la probabilidad.
  float step(float temp, float rh, float occupied, float noiseDb) {
    temps_[pos_ % kHist] = temp;
    occ_[pos_ % NN_VENTANA] = occupied;
    noise_[pos_ % NN_VENTANA] = noiseDb;
    ++pos_;
    if (count_ < 0xFFFF) ++count_;
    historyReady = count_ >= kHist;
    float rise = 0, occ10 = 0, noise10 = noiseDb;
    probability = 0;
    if (historyReady) {
      rise = temp - temps_[pos_ % kHist];          // el mas antiguo del buffer: hace 10 muestras
      for (int i = 0; i < NN_VENTANA; ++i) occ10 += occ_[i];
      occ10 /= NN_VENTANA;
      float e = 0;
      for (int i = 0; i < NN_VENTANA; ++i) e += powf(10.0f, noise_[i] / 10.0f);
      noise10 = 10.0f * log10f(e / NN_VENTANA);
    }
    // La red solo responde la pregunta para la que se entreno: sala ocupada y por debajo del limite.
    if (historyReady && occupied > 0 && temp < limitC) {
      float x[NN_IN] = {temp - limitC, rise, rh, occ10, noise10};
      probability = infer(x);
    }
    return probability;
  }

  bool warning() const { return probability >= NN_UMBRAL; }

 private:
  static constexpr int kHist = NN_VENTANA + 1;
  float temps_[kHist] = {0}, occ_[NN_VENTANA] = {0}, noise_[NN_VENTANA] = {0};
  unsigned pos_ = 0, count_ = 0;
};
