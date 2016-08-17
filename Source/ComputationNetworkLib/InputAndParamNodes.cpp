//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#include "Basics.h"
#include "InputAndParamNodes.h"
#include "File.h"        // for LoadMatrixFromTextFile()
#include "TensorShape.h" // for SmallVector<>

#include <string>

namespace Microsoft { namespace MSR { namespace CNTK {

// -----------------------------------------------------------------------
// LearnableParameter (/*no input*/)
// represents weight matrices and biases
// TODO: add -Node to the class name
// -----------------------------------------------------------------------

template <class ElemType>
void LearnableParameter<ElemType>::InitShape(const TensorShape& shape)
{
    SetDims(shape, false);
    UpdateFunctionValuesSize(); // this allocates the matrix
    Value().Invalidate();
}

// constructor from config
// Parameterization is a little wicked. An older version required to specify the type of initialization
// ("uniform|fixedValue|gaussian|fromFile|fromLiteral") and then a parameter with a matching name.
// Now, only the matching parameter is sufficient, making it less verbose.
//  - init="uniform|gaussian" (random init, scaled by arg initValueScale)
//  - init="zero"
//  - initValue=scalar --> initialize from this value
//  - initValue=array or nested array --> initialize from this value, infer dimensions  --TODO: not implemented yet
//  - initFromFilePath="..." --> read from a data file. This infers the dimensions from the file.
// deprecated:
//  - init="fixedValue",  value from 'value'            --deprecated in favor of just specifying initValue
//  - init="fromFile",    value from 'initFromFilePath' --deprecated in favor of just specifying 'initFromFilePath'
//  - init="fromLiteral", value from 'initFromLiteral'  --deprecated in favor of initValue=array expression
// The forms that infer the dimensions have different BrainScript names. TODO: need one for fromFile
// TODO: All forms that require specified dimensions but contain zeroes (to be updated by graph)
//       will need to do deferred initialization, or have a way to repeat it.
template <class ElemType>
LearnableParameter<ElemType>::LearnableParameter(const ScriptableObjects::IConfigRecordPtr configp) :
    LearnableParameter(configp->Get(L"deviceId"), L"<placeholder>", configp->Get(L"shape"))
{
    AttachInputsFromConfig(configp, this->GetExpectedNumInputs()); // (we have none; this checks that none are provided)
    // Parameter{dims, other optional parameters: learningRateMultiplier=[1|0|float], init=[uniform|gaussian|], initValueScale=[1|float], initValue=[''|float], initFromFilePath=[''|string]}

    // constant vs. parameter (with optional LR scaling)
    if (configp->Exists(L"learningRateMultiplier"))
        SetLearningRateMultiplier(configp->Get(L"learningRateMultiplier"));
    else if (configp->Exists(L"needsGradient") || configp->Exists(L"needGradient") || configp->Exists(L"computeGradient"))
        InvalidArgument("Deprecated parameter names needsGradient|needGradient|computeGradient are not supported in BrainScript. Use learningRateMultiplier instead.");

    // initialization
    wstring initString = configp->Get(L"init");
    wstring initFromFilePath = configp->Get(L"initFromFilePath");
    let& initValue = configp->Get(L"initValue");   // may be empty string, scalar, or array
    // infer the type of the initial value from what other optional args are given
    if (initString.empty())
    {
        if (!initFromFilePath.empty())                       // 'initFromFilePath' given --> initialize from file
            initString = L"fromFile"; // (note: this is only used internally; external use is deprecated)
        else if (!initValue.Is<ScriptableObjects::String>()) // 'initValue' given (not an empty string) --> initialize from value
        {
            if (initValue.Is<ScriptableObjects::Double>())
                initString = L"fromValue"; // (note: this is only used internally)
            else if (initValue.Is<ScriptableObjects::ConfigArray>())
                initString = L"fromValueArray"; // (note: this is only used internally)
            else
                InvalidArgument("'initValue' must be numerical");
        }
        else if (!initValue.AsRef<ScriptableObjects::String>().empty()) // it's a string: must be empty
            InvalidArgument("LearnableParameter: 'initValue' must be an empty string or not a string.");
        else  // no pertinent optional arguments given: default to 'uniform'
            initString = L"uniform"; // default is uniform
    }
    // deferred variants
    // Deferred means that this kind of initialization is allowed when some dimensions are unspecified, and thus happens during Validate().
    if (initString == L"uniform" || initString == L"gaussian") // random init
    {
        m_initString = initString;
        // TODO: add more randomization types, and use a more meaningful scaling
        // Keras uses "normal" instead of "gaussian". We can use that here too to denote the one with sane scaling, and deprecate "gaussian" with a warning.
        static unsigned long randomSeed = 1;
        int forcedRandomSeed = configp->Get(L"randomSeed"); // forcing a specific random seed is useful for testing to get repeatable initialization independent of evaluation order
        m_randomSeed = forcedRandomSeed < 0 ? randomSeed++ : (unsigned long)forcedRandomSeed;
        m_initValueScale = configp->Get(L"initValueScale");
        m_initOnCPUOnly = configp->Get(L"initOnCPUOnly");
    }
    else if (initString == L"zero")
    {
        m_initString = L"fromValue";
        m_initValue = 0;
    }
    else if (initString == L"fromValue") // from 'initValue'
    {
        m_initString = initString;
        m_initValue = initValue;
    }
    // non-deferred variants
    // For the dimensions are always known at this point, so we don't need/want to have to save all those parameters.
    else if (initString == L"fromValueArray") // from 'initValue' which has array form
        InvalidArgument("'initValue' for arrays not yet implemented"); // array not yet implemented
    else if (initString == L"fromFile") // load from 'iniFromFilePath'
    {
        if (initFromFilePath.empty())
            RuntimeError("initFromFilePath parameter must be provided when using \"fromFile\" initialization method");
        InitFromFile(initFromFilePath);
        m_initString.clear();
    }
    // legacy
    else if (initString == L"fixedValue") // deprecated. Use initValue=... instead
    {
        m_initString = L"fromValue";
        m_initValue = (ElemType)configp->Get(L"value");
    }
    else if (initString == L"fromLiteral") // deprecated. Use initValue=array instead
    {
        wstring initFromLiteral = configp->Get(L"initFromLiteral");
        if (initFromLiteral.empty())
            RuntimeError("initFromLiteral parameter must be provided when using \"fromLiteral\" initialization method");
        size_t numRows, numCols;
        auto array = File::LoadMatrixFromStringLiteral<ElemType>(msra::strfun::utf8(initFromLiteral), numRows, numCols);
        InitFromArray(array, numRows, numCols);
        m_initString.clear();
    }
    else
        RuntimeError("init must be one of the values of [ uniform | gaussian | fixedValue | fromFile ]");

    // initialize
    // This will be repeated if the matrix gets resized due to dimension inference.
    LazyInitParameters();

    if (!m_initString.empty())
        fprintf(stderr, "%ls: Initializating Parameter[%s] as %ls later when dimensions are fully known.\n", NodeDescription().c_str(), string(GetSampleLayout()).c_str(), m_initString.c_str());
}

// variant of above from NDL. Must be called right after plain constructor.
// This overwrites any pending deferred initialization with a new one.
// Initialization is done immediately if all dimensions are already known, otherwise kept pending.
template <class ElemType>
void LearnableParameter<ElemType>::PostInitParameters(const wstring& initString, // "uniform"|"gaussian"|"fixedValue"
                                                      ElemType initValue,        //  scale   | scale    | value
                                                      unsigned long randomSeed /*= 0*/,
                                                      bool initOnCPUOnly /*= false*/)
{
    if (initString == L"uniform" || initString == L"gaussian") // random init
    {
        m_initString = initString;
        m_randomSeed = randomSeed;
        m_initValueScale = initValue;
        m_initOnCPUOnly = initOnCPUOnly;
    }
    else if (initString == L"fixedValue") // from constant value
    {
        m_initString = L"fromValue";
        m_initValue = initValue;
    }
    else
        LogicError("PostInitParameters: invalid init string '%ls'", m_initString.c_str());

    // initialize
    // This will be repeated if the matrix gets resized due to dimension inference.
    LazyInitParameters();

    if (!m_initString.empty())
        fprintf(stderr, "%ls: Initializating Parameter[%s] as %ls later when dimensions are fully known.\n", NodeDescription().c_str(), string(GetSampleLayout()).c_str(), m_initString.c_str());
}

// initialize with random numbers
// if 'initOnCPUOnly' then always init on CPU, making initialization consistent across both (for testing)
template <class ElemType>
void LearnableParameter<ElemType>::InitRandom(const bool uniformInit,
                                                const unsigned long randomSeed,
                                                const ElemType initValueScale,
                                                bool initOnCPUOnly)
{
    // fprintf(stderr, "%d x %d: %d  %ls\n", (int)GetNumRows(), (int)GetNumCols(), (int)randomSeed, NodeName().c_str());

    // the random seed offset is set via the "randomSeedOffset" parameter in config
    if (initOnCPUOnly)
        Value().TransferToDeviceIfNotThere(CPUDEVICE, true);
#if 1   // this more complex version is needed to repro test cases generated with an older version
    auto& value = GetSampleLayout().GetRank() > 2 ? Value() : ValueAsMatrix();
#else
    auto& value = Value();
#endif
    if (uniformInit)
    {
        // TODO: move these hidden extra factors out from here and into NDL, and make them visible in BS
        ElemType randRange = 0.05f * initValueScale;
        value.SetUniformRandomValue(-randRange, randRange, randomSeed);
    }
    else
    {
        size_t inputSize = value.GetNumCols();
        ElemType randInitstd = 0.2f * initValueScale / sqrt(ElemType(inputSize));
        value.SetGaussianRandomValue(0, randInitstd, randomSeed);
    }
    if (initOnCPUOnly)
        Value().TransferToDeviceIfNotThere(m_deviceId, true);
}

// initialize by reading a matrix from a text file
template <class ElemType>
void LearnableParameter<ElemType>::InitFromFile(const wstring& initFromFilePath)
{
    size_t numRows, numCols;
    auto array = File::LoadMatrixFromTextFile<ElemType>(initFromFilePath, numRows, numCols);
    InitFromArray(array, numRows, numCols);
}

// initialize by reading a matrix from a text file
template <class ElemType>
void LearnableParameter<ElemType>::InitFromArray(const std::vector<ElemType>& array, size_t numRows, size_t numCols)
{
    // infer tensor dimensions from input file if not set
    // Note: The mapping of dimensions of the input matrix to tensor dimensions are somewhat confusing.
    //       The file contains a 2D matrix (one row per text line) that is saved into our column-major representation.
    //       That representation is then reshaped into a column-major tensor.
    if (GetSampleLayout().GetNumElements() == 0)    // at least one dimension is 0
    {
        auto dims = GetSampleLayout().GetDims();
        // infer rank
        if (dims.size() == 0)
            dims.push_back(0);
        if (dims.size() == 1 && numCols != 1)
            dims.push_back(0);
        // infer #rows
        if (dims[0] == 0)           // infer row dimension as input matrix row dimension
            dims[0] = numRows;      // (if already set, then mismatch will be caught in VerifyDataSize() below)
        // infer #cols: product of all dimensions but the first must match matrix #cols; if there is a single 0 position, we infer it
        size_t zeroDim = 0;         // 0 means not found
        size_t prod = 1;
        for (size_t k = 1; k < dims.size(); k++)
        {
            auto dim = dims[k];
            if (dim != 0)
                prod *= dim;
            else if (zeroDim == 0)
                zeroDim = k;
            else
                InvalidArgument("%ls %ls operation's specified shape [%s] cannot be inferred: Too many unknown dimensions.", NodeName().c_str(), OperationName().c_str(), string(GetSampleLayout()).c_str());
        }
        if (zeroDim != 0)   // we found a zero
        {
            dims[zeroDim] = numCols / prod;
            if (prod * dims[zeroDim] != numCols)
                InvalidArgument("%ls %ls operation's specified shape [%s] cannot be inferred: Tensor shape cannot hold a [%d x %d] matrix.", NodeName().c_str(), OperationName().c_str(), string(GetSampleLayout()).c_str(), (int)numRows, (int)numCols);
        }
        SetDims(TensorShape(dims), false);
    }

    // BUGBUG: We should allow to read an arbitrary tensor from a single-column file.
    //         Currently, this would cause a matrix/tensor dimension mismatch. --TODO: Is this comment up-to-date?
    Value().SetValue(numRows, numCols, m_deviceId, const_cast<ElemType*>(array.data()), matrixFlagNormal);
    // TODO: Get rid of that const_cast, as soon as after Ryan's Matrix-lib refactoring separated out SetValue() from external vs. from deep copy
    VerifyDataSize(Value());      // sanity check
}

// TODO: Move this error check there, since this is called only from one place.
template <class ElemType>
void LearnableParameter<ElemType>::ReviseFromFile(const std::wstring& reviseFromFilePath)
{
    try
    {
        InitFromFile(reviseFromFilePath);
    }
    catch (const std::exception & e)
    {
        RuntimeError("ReviseFromFile: Failed to reload %ls %ls operation from file %ls: %s", NodeName().c_str(), OperationName().c_str(), reviseFromFilePath.c_str(), e.what());
    }
}

template <class ElemType>
void LearnableParameter<ElemType>::Save(File& fstream) const /*override*/
{
    if (!m_initString.empty())
        LogicError("LearnableParameter: Cannot Save() before deferred initialization has completed.");
    Base::Save(fstream);
    fstream << m_learningRateMultiplier;
    m_sampleLayout.Save(fstream);
    fstream << Value();
}

template <class ElemType>
void LearnableParameter<ElemType>::Load(File& fstream, size_t modelVersion) /*override*/
{
    Base::Load(fstream, modelVersion);

    TensorShape sampleLayout;

    if (modelVersion >= CNTK_MODEL_VERSION_3)
    {
        fstream >> m_learningRateMultiplier;
        sampleLayout.Load(fstream);
    }
    else // legacy format(s)
    {
        bool parameterUpdateRequired;
        fstream >> parameterUpdateRequired;
        SetLearningRateMultiplier((float)parameterUpdateRequired);

        size_t rows, cols;
        fstream >> rows >> cols;
        if (rows != 0) // legacy file format
            sampleLayout = TensorShape(rows, cols);
        else
        {
            sampleLayout.Load(fstream, /*acceptLegacyFormat=*/true);
            if (cols > 1) // in some legacy format, last tensor dimension was split off as an explicit column dimension
                sampleLayout.AppendInPlace(sampleLayout.GetRank(), cols);
        }
    }

    LoadValue(fstream);
    SetDims(sampleLayout, false); // note: call this after LoadValue() since LoadValue() overwrites m_sampleLayout
    VerifyDataSize(Value());      // sanity check

    m_initString.clear(); // deferred initialization not possible after loading
}

template <class ElemType>
/*virtual*/ void LearnableParameter<ElemType>::CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const /*override*/
{
    Base::CopyTo(nodeP, newName, flags);
    if (flags & CopyNodeFlags::copyNodeValue)
    {
        auto node = dynamic_pointer_cast<LearnableParameter<ElemType>>(nodeP);
        node->m_initString     = m_initString;
        node->m_randomSeed     = m_randomSeed;
        node->m_initValueScale = m_initValueScale;
        node->m_initOnCPUOnly  = m_initOnCPUOnly;
        node->m_initValue      = m_initValue;
    }
}

// computation functions don't do anything for parameter nodes
template <class ElemType>
/*virtual*/ void LearnableParameter<ElemType>::UpdateFunctionMBSize() /*override*/
{
    if (!m_initString.empty())
        LogicError("LearnableParameter: Deferred initialization has not been completed until first call to UpdateFunctionMBSize().");
}

template <class ElemType>
/*virtual*/ void LearnableParameter<ElemType>::ForwardProp(const FrameRange&) /*override*/
{
}

template <class ElemType>
/*virtual*/ void LearnableParameter<ElemType>::BackpropTo(const size_t /*inputIndex*/, const FrameRange&) /*override*/
{
    LogicError("%ls %ls operation is a leaf node. BackpropTo() should never be called.", NodeName().c_str(), OperationName().c_str());
}

template <class ElemType>
/*virtual*/ void LearnableParameter<ElemType>::Validate(bool isFinalValidationPass) /*override*/
{
    //fprintf(stderr, "Validate %ls: called in init state '%ls' with dims [%s]\n", NodeDescription().c_str(), m_initString.c_str(), string(GetSampleLayout()).c_str());
    Base::Validate(isFinalValidationPass);
    m_pMBLayout = nullptr; // this node does not hold mini-batch data

    // lazy init if we got a dimension now
#if 0 // fake old buggy behavior before deferred initialization
    if (isFinalValidationPass && !m_initString.empty() && (m_initString != L"fromValue" || m_initValue != 0))
    {
        fprintf(stderr, "Validate: deferred '%ls' initialization patched to fromValue 0 for back compat\n", m_initString.c_str());
        m_initString = L"fromValue";
        m_initValue = 0;
    }
#endif
#if 0
    // We call this here and in Validate(true), since we don't know which gets called first.
    // TODO: Actually this should never be needed, because each time dimensions change, we init.
    //       So if we get here without fully-known dimensions, this call won't do anything either.
    if (isFinalValidationPass)
        LazyInitParameters();
#endif
}

// deferred initialization
// We support a feature that some dimensions can be specified as 0, and get inferred.
// This is only possible for initialization methods that do not come with their own dimensions
// (such as initialization from an array literal).
// When initialization succeeded (all dimensions known), the pending initialization is cleared.
// This is called from constructor and InferInputDimsFrom().
// BUGBUG: We cannot really enforce the calling sequence. Save() verifies that this has been cleared.
//         Note that this may be called AFTER Validate(true) (still during validation, but after final validation of this node).
template <class ElemType>
void LearnableParameter<ElemType>::LazyInitParameters()
{
    // if no lazy init pending then we are done
    if (m_initString.empty())
        return;
    // if not all dimensions are known yet, we cannot proceed: keep it pending
    if (GetSampleLayout().GetNumElements() == 0)
        return;
    // OK, proceed
    if (m_initString == L"fromValue")
    {
        fprintf(stderr, "%ls: Initializing Parameter[%s] <- %f.\n", NodeDescription().c_str(), string(GetSampleLayout()).c_str(), m_initValue);
        Value().SetValue(m_initValue);
    }
    else if (m_initString == L"uniform" || m_initString == L"gaussian")
    {
        fprintf(stderr, "%ls: Initializing Parameter[%s] <- %ls(seed=%d, scale=%f, onCPU=%s).\n", NodeDescription().c_str(), string(GetSampleLayout()).c_str(), m_initString.c_str(), (int)m_randomSeed, m_initValueScale, m_initOnCPUOnly ? "true" : "false");
        InitRandom((m_initString == L"uniform"), m_randomSeed, m_initValueScale, m_initOnCPUOnly);
    }
    else
        LogicError("LearnableParameter: Invalid value of m_initString '%ls' for deferred initialization for %ls.", m_initString.c_str(), NodeDescription().c_str());
    // and remember that we are done
    m_initString.clear();
}

// called from ComputationNode::ValidateInferInputDimsFrom()
// In case of an error, this function just backs out without updating.
// The caller must verify the dimensions.
// This is a bit weird since it is called after this node has been Validated once.
template <class ElemType>
void LearnableParameter<ElemType>::InferInputDimsFrom(const TensorShape& otherShape)
{
//fprintf(stderr, "InferInputDimsFrom %ls: called in init state '%ls' with dims [%s], offered new dims [%s]\n", NodeDescription().c_str(), m_initString.c_str(), string(GetSampleLayout()).c_str(), string(otherShape).c_str());
    const auto& thisShape = GetSampleLayout();

    // see where we stand with our shape
    bool hasMissingDims = thisShape.GetRank() == 0 || thisShape.GetNumElements() == 0;
    if (!hasMissingDims) // all there--nothing to infer
        return;
    
    // infer at least one dimension
    if (otherShape.GetRank() == 0 || otherShape.GetNumElements() == 0)
        return; // LogicError("ValidateInferInputDimsFrom: Inferred dimensions must not be empty.");

    if (m_initString.empty())
        LogicError("InferInputDimsFrom: Attempted to infer dimensions, with initialization completed or no deferred initialization pending.");

    // if no dimensions have been set at all, copy otherShape
    // Don't verify dimensions in this case, because the node may have explicitly been defined as a vector of 0 elements.
    bool hasAnyDim = false;
    for (auto dim : thisShape.GetDims())
        hasAnyDim |= dim != 0;
    if (!hasAnyDim)          // just use it straight
        InitShape(otherShape);
    else if (hasMissingDims) // we got a pre-existing shape: If it has zeroes, we fill them in from otherShape
    {
        if (thisShape.GetRank() != 0 && thisShape.GetRank() != otherShape.GetRank())
            return; // LogicError("ValidateInferInputDimsFrom: Inferred dimensions must match in rank.");
        SmallVector<size_t> newDims = thisShape.GetDims();
        for (size_t i = 0; i < thisShape.GetRank(); i++)
            if (newDims[i] == 0)
                newDims[i] = otherShape[i];
        InitShape(TensorShape(newDims));
    }
    fprintf(stderr, "%ls operation: Tensor shape was inferred as [%s].\n", NodeDescription().c_str(), string(GetSampleLayout()).c_str());

    // initialize the values
    // We call this here and in Validate(true), since we don't know which gets called first.
    // Note: It seems that this is not necessary, and that Validate(true) is only called after inference.
#if 0 // fake old buggy behavior before deferred initialization
    if (m_initString != L"fromValue" || m_initValue != 0)
    {
        fprintf(stderr, "InferInputDimsFrom: deferred '%ls' initialization patched to fromValue 0 for back compat\n", m_initString.c_str());
        m_initString = L"fromValue";
        m_initValue = 0;
    }
#endif
    LazyInitParameters();
}

template <class ElemType>
/*virtual*/ void LearnableParameter<ElemType>::DumpNodeInfo(const bool printValues, const bool printMetadata, File& fstream) const /*override*/
{
    if (printMetadata)
    {
        Base::DumpNodeInfo(printValues, printMetadata, fstream);

        char str[4096];
        sprintf(str, "[%zu,%zu]  ", GetAsMatrixNumRows(), GetAsMatrixNumCols());
        fstream << string(str);
        sprintf(str, "learningRateMultiplier=%f  NeedsGradient=%s", m_learningRateMultiplier, m_learningRateMultiplier>0 ? "true" : "false"); // TODO: update NDL to accept a better matching name as well
        fstream << string(str);
    }

    PrintNodeValuesToFile(printValues, printMetadata, fstream);
}

template <class ElemType>
/*virtual*/ void LearnableParameter<ElemType>::FreezeParameters() /*override*/ // from IFreezable
{
    SetLearningRateMultiplier(0);
}

template class LearnableParameter<float>;
template class LearnableParameter<double>;

}}}
