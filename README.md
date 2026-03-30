# Cloth [WIP]

**A low-level, compiled, object-oriented systems programming language**

Cloth is designed to combine the **clarity and structure of Java** with the **control and performance of C**, without the complexity, inconsistency, and historical baggage of C++.

---
![GitHub Actions Workflow Status](https://img.shields.io/github/actions/workflow/status/Cloth-Foundation/cCloth/gradle.yml?style=for-the-badge)
![GitHub License](https://img.shields.io/github/license/Cloth-Foundation/cCloth?style=for-the-badge)
![GitHub Repo stars](https://img.shields.io/github/stars/Cloth-Foundation/cCloth?style=for-the-badge)
![GitHub forks](https://img.shields.io/github/forks/Cloth-Foundation/cCloth?style=for-the-badge)
![GitHub contributors](https://img.shields.io/github/contributors/Cloth-Foundation/cCloth?style=for-the-badge)

## What is Cloth?
Cloth is an object-oriented, compiled language, inspired by C++, Java, and C#. It introduces a clean and clear syntax
that is both easy to learn and easy to use. The main goal of Cloth is to be a simple, yet powerful enough to be used
as a systems programming language. 

## Why does Cloth exist?
Cloth was made out of a frustration with the lack of a simple, yet powerful, non-garbage collected, low-level language.
It aims for the low-level accessibility of C, while having a Java-like class encapsulation system, while maintaining
the accessibility of C#.

Some parts of Cloth are purposely verbose to make it not only easy to learn, but easy to maintain. The Cloth Foundation
is currently working on a language standard for Cloth to keep the language consistent, while also making sure that previous
codebases of Cloth are still usable will little to no refactoring.

## What is the status of Cloth?
Cloth is currently in the early stages of development.

## What does Cloth look like?

```cloth
module cloth;

import std.io;

public class MyClass(int number?) {
    const i32 myInt? { public get; };

    public MyClass {
        this.myInt = number;
    }

    public func convertIntToFloat() :> float {
        return getMyInt() as float;
    }
}
```