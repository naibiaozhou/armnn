//
// Copyright © 2017 Arm Ltd. All rights reserved.
// SPDX-License-Identifier: MIT
//
#include "OnnxParser.hpp"

#include <armnn/ArmNN.hpp>
#include <armnn/Utils.hpp>
#include <VerificationHelpers.hpp>

#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

#include <boost/format.hpp>

#include <numeric>

using namespace armnn;

namespace armnnOnnxParser
{
namespace
{
void CheckValidDataType(std::initializer_list<onnx::TensorProto::DataType> validInputTypes,
                        const onnx::TensorProto::DataType actualValue,
                        const char* validExpr,
                        std::string nodeName,
                        std::string tensorName,
                        const armnn::CheckLocation& location)
{
    bool isValid = std::any_of(validInputTypes.begin(),
                               validInputTypes.end(),
                               [&actualValue](onnx::TensorProto::DataType x) { return x == actualValue; } );
    if (!isValid)
    {
        throw ParseException(
            boost::str(
                boost::format("Datatype %1% is not valid for tensor '%2%' of node '%3%', not in {%4%}. %5%") %
                              onnx::TensorProto::DataType_Name(actualValue) %
                              tensorName %
                              nodeName %
                              validExpr %
                              location.AsString()));
    }
}

#define CHECK_VALID_DATATYPE(NODE, TENSOR, ACTUAL, ...) \
CheckValidDataType({__VA_ARGS__}, ACTUAL, #__VA_ARGS__, NODE, TENSOR, CHECK_LOCATION())

using StrTypeListPair = std::pair<const char*, std::initializer_list<onnx::TensorProto::DataType>>;
#define STR_LIST(...) StrTypeListPair(#__VA_ARGS__, {__VA_ARGS__})

template <typename Callable>
void ReadMandatoryNodeAttributeImpl(const onnx::NodeProto& node,
                                    const std::string& attribName,
                                    onnx::AttributeProto::AttributeType expectedType,
                                    Callable callable)
{
  auto attribs = node.attribute();
  int attriNum = 0;
  while (attriNum < node.attribute_size())
  {
      if (attribs.Get(attriNum).name() == attribName)
      {
          if (attribs.Get(attriNum).type() == expectedType)
          {
              callable(attribs.Get(attriNum));
          }
          else
          {
              throw ParseException(boost::str(boost::format(
                  "Attribute %1% of node %2% expected to have %3% as onnx::AttributeProto::AttributeType, "
                  "but found %4% instead %5%")
                  % attribName
                  % node.name()
                  % onnx::AttributeProto::AttributeType_Name(expectedType)
                  % onnx::AttributeProto::AttributeType_Name(attribs.Get(attriNum).type())
                  % CHECK_LOCATION().AsString()));
          }
          break;
      }
      ++attriNum;
  }
  if (attriNum == node.attribute_size())
  {
      throw ParseException(boost::str(boost::format("Could not find required attribute %1% in node %2% %3%")
          % attribName % node.name() % CHECK_LOCATION().AsString()));
  }
}

template <typename Callable>
void ReadOptionalNodeAttributeImpl(const onnx::NodeProto& node,
                                   const std::string& attribName,
                                   onnx::AttributeProto::AttributeType expectedType,
                                   Callable callable)
{
    auto attribs = node.attribute();
    for (int attriNum = 0; attriNum < node.attribute_size(); ++attriNum)
    {
        if (attribs.Get(attriNum).name() == attribName)
        {
            if (attribs.Get(attriNum).type() == expectedType)
            {
                callable(attribs.Get(attriNum));
            }
            else
            {
                throw ParseException(boost::str(boost::format(
                    "Attribute %1% of node %2% expected to have %3% as onnx::AttributeProto::AttributeType, "
                    "but found %4% instead %5%")
                    % attribName
                    % node.name()
                    % onnx::AttributeProto::AttributeType_Name(expectedType)
                    % onnx::AttributeProto::AttributeType_Name(attribs.Get(attriNum).type())
                    % CHECK_LOCATION().AsString()));
            }
        }
    }
}

std::vector<uint32_t> ReadMandatoryNodeUint32ListAttribute(const onnx::NodeProto& node,
                                                           const std::string& name)
{
    std::vector<uint32_t> attriList;
    ReadMandatoryNodeAttributeImpl(node, name, onnx::AttributeProto::INTS,
        [&attriList](const onnx::AttributeProto& attrValue)
    {
        for (int attriNum = 0; attriNum < attrValue.ints_size(); ++attriNum)
        {
            attriList.push_back(CHECKED_NON_NEGATIVE(CHECKED_INT32(attrValue.ints().Get(attriNum))));
        }
    });
    return attriList;
}

uint32_t ReadOptionalNodeUint32Attribute(const onnx::NodeProto& node,
                                         const std::string& name,
                                         const uint32_t defaultVal = 0u)
{
    uint32_t attribValue = defaultVal;
    ReadOptionalNodeAttributeImpl(node, name, onnx::AttributeProto::INT,
        [&attribValue](const onnx::AttributeProto& attrValue)
    {
        attribValue = CHECKED_NON_NEGATIVE(CHECKED_INT32((attrValue.i())));
    });
    return attribValue;
}

std::vector<uint32_t> ReadOptionalNodeUint32ListAttribute(const onnx::NodeProto& node,
                                                          const std::string& name)
{
    std::vector<uint32_t> attriList;
    ReadOptionalNodeAttributeImpl(node, name, onnx::AttributeProto::INTS,
        [&attriList](const onnx::AttributeProto& attrValue)
    {
        for (int attriNum = 0; attriNum < attrValue.ints_size(); ++attriNum)
        {
            attriList.push_back(CHECKED_NON_NEGATIVE(CHECKED_INT32(attrValue.ints().Get(attriNum))));
        }
    });

    return attriList;
}

float ReadOptionalNodeFloatAttribute(const onnx::NodeProto& node,
                                     const std::string& name,
                                     const float defaultValue = 0.0f)
{
    float attribValue = defaultValue;
    ReadOptionalNodeAttributeImpl(node, name, onnx::AttributeProto::FLOAT,
        [&attribValue](const onnx::AttributeProto& attrValue)
    {
        attribValue = attrValue.f();
    });
    return attribValue;
}

std::string ReadOptionalNodeStringAttribute(const onnx::NodeProto& node, const std::string& name)
{
    std::string attribValue = "";
    ReadOptionalNodeAttributeImpl(node, name, onnx::AttributeProto::STRING,
        [&attribValue](const onnx::AttributeProto& attrValue)
    {
        attribValue = attrValue.s();
    });
    return attribValue;
}

armnn::TensorInfo ToTensorInfo(const onnx::ValueInfoProto& info)
{
  const onnx::TensorShapeProto onnxShape = info.type().tensor_type().shape();
  std::vector<unsigned int> shapeDims;
  for (int i = 0; i < onnxShape.dim_size(); ++i)
  {
      shapeDims.push_back(CHECKED_NON_NEGATIVE(CHECKED_INT32(onnxShape.dim(i).dim_value())));
  }
  DataType type;
  switch(info.type().tensor_type().elem_type())
  {
      case onnx::TensorProto::FLOAT:
      {
        type = DataType::Float32;
        break;
      }
      case onnx::TensorProto::INT32:
      case onnx::TensorProto::INT64:
      {
          type = DataType::Signed32;
          break;
      }
      default:
      {
          throw ParseException(
              boost::str(
                  boost::format("'%1%' is not a currently supported datatype for tensor %2%."
                                " Supported dataTypes are FLOAT, INT32 and INT64.  %3%") %
                                onnx::TensorProto::DataType_Name(
                                    static_cast<onnx::TensorProto::DataType>(info.type().tensor_type().elem_type())) %
                                info.name() %
                                CHECK_LOCATION().AsString() ));
      }

  }
  return TensorInfo(TensorShape(static_cast<unsigned int>(shapeDims.size()), shapeDims.data()), type);
}

std::string TensorInfoAsString(const TensorInfo& info,
                               const std::string& name,
                               const onnx::TensorProto::DataType& type)
{
    const TensorShape shape = info.GetShape();
    std::stringstream ss;
    ss << "tensor '" << name << "' contains "
       << onnx::TensorProto::DataType_Name(type)
       << " and has shape [";

    for (uint32_t i = 0; i < shape.GetNumDimensions() - 1; ++i)
    {
        ss << shape[i] << ", ";
    }
    ss << shape[shape.GetNumDimensions() - 1] << "]";
    return ss.str();
}

void CalcPadding(uint32_t inputSize, uint32_t filterSize, uint32_t stride, uint32_t* paddingFront,
                 uint32_t* paddingBack, bool isUpper)
{
    uint32_t outputSize = (inputSize + stride - 1) / stride;
    uint32_t temp = (outputSize - 1) * stride + filterSize;
    *paddingFront = (temp - inputSize) / 2;
    *paddingBack = *paddingFront;
    if((temp - inputSize) % 2 == 1)
    {
        if (isUpper)
        {
          *paddingBack += 1;
        }
        else
        {
          *paddingFront += 1;
        }
    }
}

TensorInfo ComputeReshapeInfo(const onnx::TensorProto& targetShapeTensor,
                              const TensorShape& inShape,
                              const std::string& outName)
{
    std::vector<int> targetDims;
    for(int i = 0; i < targetShapeTensor.int64_data_size(); ++i)
    {
        int val = CHECKED_INT32(targetShapeTensor.int64_data(i));
        if(val == 0)
        {
            targetDims.push_back(static_cast<int>(inShape[static_cast<uint>(i)]));
        }
        else
        {
            targetDims.push_back(val);
        }
    }

    std::vector<unsigned int> outDims(targetDims.begin(), targetDims.end());
    const auto stretchDim = std::find(targetDims.begin(), targetDims.end(), -1);
    if (stretchDim != targetDims.end())
    {
        if (std::find(std::next(stretchDim), targetDims.end(), -1) != targetDims.end())
        {
            std::stringstream ss;
            ss << "[ ";
            for(uint i = 0; i < targetDims.size() - 1; ++i)
            {
                ss << targetDims[i] << ", ";
            }
            ss << targetDims[targetDims.size() - 1] << " ]";

            throw ParseException(boost::str(
                boost::format("Error during creation of reshaped tensor '%1%'. At most one component of shape can be "
                              " -1 and here, shape is %2% %3%")
                              % outName
                              % ss.str()
                              % CHECK_LOCATION().AsString()));
        }

        auto targetNumElements = boost::numeric_cast<unsigned int>(std::accumulate(targetDims.begin(), targetDims.end(),
            -1, std::multiplies<int32_t>()));
        auto stretchIndex = static_cast<size_t>(std::distance(targetDims.begin(), stretchDim));
        outDims[stretchIndex] = inShape.GetNumElements() / targetNumElements;
    }
    TensorShape outShape = TensorShape{static_cast<unsigned int>(outDims.size()), outDims.data()};
    return TensorInfo(outShape, DataType::Float32);
}

} //namespace

const std::map<std::string, OnnxParser::OperationParsingFunction> OnnxParser::m_ParserFunctions = {
    { "BatchNormalization",    &OnnxParser::ParseBatchNormalization},
    { "GlobalAveragePool",     &OnnxParser::ParseGlobalAveragePool},
    { "AveragePool",           &OnnxParser::ParseAveragePool },
    { "Constant",              &OnnxParser::ParseConstant },
    { "MaxPool",               &OnnxParser::ParseMaxPool },
    { "Reshape",               &OnnxParser::ParseReshape },
    { "Relu",                  &OnnxParser::ParseRelu },
    { "Conv",                  &OnnxParser::ParseConv },
    { "Add",                   &OnnxParser::ParseAdd },
};

template<typename TypePair, typename Location>
void OnnxParser::ValidateInputs(const onnx::NodeProto& node,
                                TypePair validInputs,
                                const Location& location)
{
    for(auto input : node.input())
    {
        CheckValidDataType(validInputs.second,
                           m_TensorsInfo[input].m_dtype,
                           validInputs.first,
                           node.name(),
                           input,
                           location);
    }
}

#define VALID_INPUTS(NODE, VALID_INPUTS) \
    OnnxParser::ValidateInputs(NODE, \
                               VALID_INPUTS, \
                               CHECK_LOCATION())

std::vector<TensorInfo> OnnxParser::ComputeOutputInfo(std::vector<std::string> outNames,
                                                       const IConnectableLayer* layer,
                                                       std::vector<TensorShape> inputShapes)
{
    BOOST_ASSERT(! outNames.empty());
    bool needCompute = std::any_of(outNames.begin(),
                                   outNames.end(),
                                   [this](std::string name)
                                   {
                                       return (m_TensorsInfo.count(name) == 0 || m_TensorsInfo[name].m_info == nullptr);
                                   });
     std::vector<TensorInfo> outInfo;
     //if the output info(s) are not here, we need to compute them
     std::vector<TensorShape> inferredShapes;
     if(needCompute)
     {
         inferredShapes = layer->InferOutputShapes(inputShapes);
         BOOST_ASSERT(inferredShapes.size() == outNames.size());
     }
     for (uint i = 0; i < outNames.size(); ++i)
     {
         if(needCompute)
         {
             m_TensorsInfo[outNames[i]] = OnnxTensor();
             m_TensorsInfo[outNames[i]].m_info = std::make_unique<TensorInfo>(
                TensorInfo(inferredShapes[i], DataType::Float32));
         }
        outInfo.push_back(*m_TensorsInfo[outNames[i]].m_info);
     }
     return outInfo;
}

IOnnxParser* IOnnxParser::CreateRaw()
{
    return new OnnxParser();
}

IOnnxParserPtr IOnnxParser::Create()
{
    return IOnnxParserPtr(CreateRaw(), &IOnnxParser::Destroy);
}

void IOnnxParser::Destroy(IOnnxParser* parser)
{
    delete parser;
}

OnnxParser::OnnxParser()
    : m_Network(nullptr, nullptr)
{
}

void OnnxParser::ResetParser()
{
    m_Network = armnn::INetworkPtr(nullptr, nullptr);
    m_Graph = nullptr;
}

void OnnxParser::Cleanup()
{
    m_TensorConnections.clear();
    m_TensorsInfo.clear();
    m_OutputsMap.clear();
    m_OutputsFusedAndUsed.clear();
}

std::pair<ConstTensor, std::unique_ptr<float[]>> OnnxParser::CreateConstTensor(const std::string name)
{
    const TensorInfo tensorInfo = *m_TensorsInfo[name].m_info;
    onnx::TensorProto onnxTensor = *m_TensorsInfo[name].m_tensor;

    auto srcData = onnxTensor.float_data().data();
    if(tensorInfo.GetNumElements() != static_cast<uint>(onnxTensor.float_data_size()))
    {
        throw ParseException(boost::str(
            boost::format("The number of data provided (%1%) does not match the tensor '%2%' number of elements"
                          " (%3%) %4%")
                          % onnxTensor.float_data_size()
                          % name
                          % tensorInfo.GetNumElements()
                          % CHECK_LOCATION().AsString()));
    }
    std::unique_ptr<float[]> tensorData(new float[tensorInfo.GetNumElements()]);

    // Copy the value list entries into the destination
    ::memcpy(tensorData.get(),srcData, tensorInfo.GetNumBytes());

    // Const tensors requires at least a list of values
    if (tensorInfo.GetNumElements() == 0)
    {
        throw ParseException(boost::str(
            boost::format("No tensor data found for Const tensor '%1%' %2%")
                          % name
                          % CHECK_LOCATION().AsString()));
    }
    return std::make_pair(ConstTensor(tensorInfo, tensorData.get()), std::move(tensorData));
}

ModelPtr OnnxParser::LoadModelFromTextFile(const char* graphFile)
{
    FILE* fd = fopen(graphFile, "r");

    if (fd == nullptr)
    {
        throw FileNotFoundException(boost::str(
            boost::format("Invalid (null) filename %1%") % CHECK_LOCATION().AsString()));
    }

    // Parse the file into a message
    ModelPtr     modelProto = std::make_unique<onnx::ModelProto>();
    using google::protobuf::io::FileInputStream;
    std::unique_ptr<FileInputStream> input = std::make_unique<FileInputStream>(fileno(fd));
    bool                 success = google::protobuf::TextFormat::Parse(input.get(), modelProto.get());
    fclose(fd);

    if (!success)
    {
        std::stringstream error;
        error << "Failed to parse graph file";
        throw ParseException(boost::str(
            boost::format("%1% %2%") % error.str() % CHECK_LOCATION().AsString()));
    }
    return modelProto;
}

INetworkPtr OnnxParser::CreateNetworkFromTextFile(const char* graphFile)
{
    ResetParser();
    ModelPtr modelProto = LoadModelFromTextFile(graphFile);
    return CreateNetworkFromModel(*modelProto);
}


ModelPtr OnnxParser::LoadModelFromBinaryFile(const char* graphFile)
{
    FILE* fd = fopen(graphFile, "rb");

    if (fd == nullptr)
    {
        throw FileNotFoundException(boost::str(
            boost::format("Invalid (null) filename %1%") % CHECK_LOCATION().AsString()));
    }

    // Parse the file into a message
    ModelPtr modelProto = std::make_unique<onnx::ModelProto>();

    google::protobuf::io::FileInputStream  inStream(fileno(fd));
    google::protobuf::io::CodedInputStream codedStream(&inStream);
    codedStream.SetTotalBytesLimit(INT_MAX, INT_MAX);
    bool success = modelProto.get()->ParseFromCodedStream(&codedStream);
    fclose(fd);

    if (!success)
    {
        std::stringstream error;
        error << "Failed to parse graph file";
        throw ParseException(boost::str(
            boost::format("%1% %2%") % error.str() % CHECK_LOCATION().AsString()));
    }
    return modelProto;

}

INetworkPtr OnnxParser::CreateNetworkFromBinaryFile(const char* graphFile)
{
    ResetParser();
    ModelPtr modelProto = LoadModelFromBinaryFile(graphFile);
    return CreateNetworkFromModel(*modelProto);
}

ModelPtr OnnxParser::LoadModelFromString(const std::string& protoText)
{
    if (protoText == "")
    {
        throw InvalidArgumentException(boost::str(
                boost::format("Invalid (empty) string for model parameter %1%") % CHECK_LOCATION().AsString()));
    }
    // Parse the string into a message
    ModelPtr modelProto = std::make_unique<onnx::ModelProto>();
    bool success = google::protobuf::TextFormat::ParseFromString(protoText, modelProto.get());
    if (!success)
    {
        std::stringstream error;
        error << "Failed to parse graph file";
        throw ParseException(boost::str(
                boost::format("%1% %2%") % error.str() % CHECK_LOCATION().AsString()));
    }
    return modelProto;
}

INetworkPtr OnnxParser::CreateNetworkFromString(const std::string& protoText)
{
    ResetParser();
    ModelPtr modelProto = LoadModelFromString(protoText);
    return CreateNetworkFromModel(*modelProto);
}

INetworkPtr OnnxParser::CreateNetworkFromModel(onnx::ModelProto& model)
{
    m_Network = INetwork::Create();
    try
    {
        m_Graph = std::make_unique<onnx::GraphProto>(*model.mutable_graph());
        LoadGraph();
    }
    catch (const ParseException& e)
    {
        Cleanup();
        throw e;
    }
    Cleanup();
    return std::move(m_Network);
}

void OnnxParser::LoadGraph()
{
    BOOST_ASSERT(m_Graph.get() != nullptr);

    //Fill m_TensorsInfo with the shapes and value of every tensor
    SetupInfo(m_Graph->mutable_output());
    SetupInfo(m_Graph->mutable_input());
    SetupInfo(m_Graph->mutable_value_info());

    for (auto tensor : m_Graph->initializer())
    {
        m_TensorsInfo[tensor.name()].m_tensor = std::make_unique<const onnx::TensorProto>(tensor);
    }

    SetupInputLayers();
    SetupOutputLayers();

    //Detect FullyConnected layers with bias and update the FusedAndUsed map acccordingly
    DetectFullyConnected();

    //Parsing the graph
    for(size_t nodeIndex = 0; nodeIndex < static_cast<size_t>(m_Graph->node_size()); nodeIndex++)
    {
        auto node = m_Graph->node(static_cast<int>(nodeIndex));
        const std::string& operation = node.op_type();

        // check which layers we handled already (add and matmul fused as FC)
        if(operation == "MatMul" )
        {
            if(m_OutputsFusedAndUsed[nodeIndex].inputForNodes != m_OutputsFusedAndUsed[nodeIndex].fusedWithNodes.size())
            {
                //Node which can not be fused as a FullyConnected layer (used in layers as a simple matmul output)
                AddFullyConnected(node);
            }
        }
        else if (!(m_OutputsFusedAndUsed[nodeIndex].fusedWithNodes.empty()) && operation == "Add")
        {
            int matmulIndex = static_cast<int> (m_OutputsFusedAndUsed[nodeIndex].fusedWithNodes[0]);
            AddFullyConnected(m_Graph->node(matmulIndex), &node);
        }
        else if (m_OutputsFusedAndUsed[nodeIndex].fusedWithNodes.empty()) //node is not part of a fused layer
        {
            auto it = m_ParserFunctions.find(operation);
            if (it != m_ParserFunctions.end())
            {
                auto func = it->second;
                (this->*func)(node);
            }
            else
            {
                throw ParseException(boost::str(
                    boost::format("Unsupported operation %1% for node '%2%' %3%")
                    % operation
                    % node.name()
                    % CHECK_LOCATION().AsString()));
            }
        }
    }

    //Making the connections between outputs and inputs of each layers
    for (const auto& tensorCon : m_TensorConnections)
    {
        if (tensorCon.second.outputSlot != nullptr)
        {
            for (size_t inputSlotIdx = 0; inputSlotIdx < tensorCon.second.inputSlots.size(); ++inputSlotIdx)
            {
                tensorCon.second.outputSlot->Connect(*(tensorCon.second.inputSlots[inputSlotIdx]));
            }
        }
    }
}

void OnnxParser::SetupInfo(const google::protobuf::RepeatedPtrField<onnx::ValueInfoProto >* list)
{
    for (auto tensor : *list)
    {
        m_TensorsInfo[tensor.name()] = OnnxTensor();
        m_TensorsInfo[tensor.name()].m_info = std::make_unique<TensorInfo>(ToTensorInfo(tensor));
        m_TensorsInfo[tensor.name()].m_dtype =
            static_cast<onnx::TensorProto::DataType>(tensor.type().tensor_type().elem_type());
    }
}

void OnnxParser::DetectFullyConnected()
{
    m_OutputsFusedAndUsed = std::vector<UsageSummary> (static_cast<size_t>(m_Graph->node_size()), UsageSummary());
    auto matmulAndConstant = [&](const std::string& constInput,
                                 const std::string& matmulInput,
                                 int& nodeIndex)
    {
        auto matmulIt = m_OutputsMap.find(matmulInput);
        if(matmulIt != m_OutputsMap.end()  && matmulIt->second.first->op_type() == "MatMul"
            && m_TensorsInfo[constInput].isConstant())
        {
            nodeIndex = matmulIt->second.second;
            return true;
        }
        return false;
    };

    for(int nodeIndex = 0; nodeIndex < m_Graph->node_size(); nodeIndex++)
    {
        const onnx::NodeProto* node = &m_Graph->node(nodeIndex);
        for (const std::string& output : node->output())
        {
            m_OutputsMap[output] = std::make_pair(node, nodeIndex);
        }

        for (const std::string& input : node->input()) //count how many time a node is used as input
        {
            auto matmulIt = m_OutputsMap.find(input);
            if(matmulIt != m_OutputsMap.end()){
                ++m_OutputsFusedAndUsed[static_cast<size_t>(matmulIt->second.second)].inputForNodes; //node used
            }
        }

        if (node->op_type() == "Add")
        {
            int matmulIndex = 0;
            if (matmulAndConstant(node->input(0), node->input(1), matmulIndex) ||
                matmulAndConstant(node->input(1), node->input(0), matmulIndex))
            {
                //matmul and add were fused
                m_OutputsFusedAndUsed[static_cast<size_t>(matmulIndex)].fusedWithNodes
                                                                       .push_back(static_cast<size_t>(nodeIndex));

                m_OutputsFusedAndUsed[static_cast<size_t>(nodeIndex)].fusedWithNodes
                                                                     .push_back(static_cast<size_t>(matmulIndex));
            }
        }
    }

    for (auto output: m_Graph->output()) { //Add usages as output of the graph in count of usages
        auto matmulIt = m_OutputsMap.find(output.name());
        if(matmulIt != m_OutputsMap.end()){
            ++m_OutputsFusedAndUsed[static_cast<size_t>(matmulIt->second.second)].inputForNodes;
        }
    }
}

template<typename Location>
void OnnxParser::GetInputAndParam(const onnx::NodeProto& node,
                                  std::string* inputName,
                                  std::string* constName,
                                  const Location& location)
{
    int cstIndex;
    if (m_TensorsInfo[node.input(0)].isConstant())
    {
        cstIndex = 0;
    }
    else if (m_TensorsInfo[node.input(1)].isConstant())
    {
        cstIndex = 1;
    }
    else
    {
        throw ParseException(boost::str(
            boost::format("One of the input tensors ('%1%' or '%2%') should be constant in node '%3%' %4%")
                          % node.input(0)
                          % node.input(1)
                          % node.name()
                          % location.AsString()));
    }
    if(constName)
    {
        *constName = node.input(cstIndex);
    }
    if(inputName)
    {
        *inputName = node.input(!cstIndex);
    }
}

template<typename Location>
void OnnxParser::To1DTensor(const std::string& name, const Location& location)
{
    TensorShape shape = m_TensorsInfo[name].m_info->GetShape();
    std::vector<uint32_t> newShape;
    for(uint i = 0; i < shape.GetNumDimensions() - 1; ++i)
    {
        if(shape[i] != 1)
        {
            throw ParseException(boost::str(
                boost::format("Only tensors with shape [1, ..., 1, X] can be converted to 1D and %1% %2%")
                             % TensorInfoAsString(*m_TensorsInfo[name].m_info, name, m_TensorsInfo[name].m_dtype)
                             % location.AsString()));
        }
    }
    newShape.push_back(shape[shape.GetNumDimensions() - 1]);

    m_TensorsInfo[name].m_info->SetShape(TensorShape(static_cast<unsigned int>(newShape.size()), newShape.data()));
}

void OnnxParser::AddFullyConnected(const onnx::NodeProto& matmulNode, const onnx::NodeProto* addNode)
{

    // find matmul inputs
    std::string weightName;
    std::string inputName;
    CHECK_VALID_SIZE(static_cast<size_t>(matmulNode.input_size()), 2);
    CHECK_VALID_SIZE(static_cast<size_t>(matmulNode.output_size()), 1);
    VALID_INPUTS(matmulNode, STR_LIST(onnx::TensorProto::FLOAT));

    GetInputAndParam(matmulNode, &inputName, &weightName, CHECK_LOCATION());

    FullyConnectedDescriptor desc;
    desc.m_BiasEnabled = addNode != nullptr;

    IConnectableLayer* layer = nullptr;
    if(desc.m_BiasEnabled)
    {
        // find bias const
        std::string biasName;
        CHECK_VALID_SIZE(static_cast<size_t>(addNode->input_size()), 2);
        CHECK_VALID_SIZE(static_cast<size_t>(addNode->output_size()), 1);
        VALID_INPUTS(*addNode, STR_LIST(onnx::TensorProto::FLOAT));

        GetInputAndParam(*addNode, nullptr, &biasName, CHECK_LOCATION());

        //Output shape is [1, weights[1]] and 1d vec in ONNX can be [1,X] so we convert biases to "armnn" 1D
        To1DTensor(biasName, CHECK_LOCATION());
        TensorInfo weightInfo = *m_TensorsInfo[weightName].m_info;
        TensorInfo biasInfo = *m_TensorsInfo[biasName].m_info;

        if (weightInfo.GetShape()[1] != biasInfo.GetShape()[0])
        {
            throw ParseException(boost::str(
                boost::format("Shape of weights '%1%' and bias of following Add node '%2%' do not match : %3%"
                              " and %4% ( /!\\ bias should be a 1D tensor) %5%")
                              % weightName
                              % addNode->name()
                              % TensorInfoAsString(*m_TensorsInfo[weightName].m_info,
                                                   weightName,
                                                   m_TensorsInfo[weightName].m_dtype)
                              % TensorInfoAsString(*m_TensorsInfo[biasName].m_info, biasName,
                                                   m_TensorsInfo[biasName].m_dtype )
                              % CHECK_LOCATION().AsString()));
        }
        layer = m_Network->AddFullyConnectedLayer(desc,
                                                  CreateConstTensor(weightName).first,
                                                  CreateConstTensor(biasName).first,
                                                  matmulNode.name().c_str());
        BOOST_ASSERT(layer != nullptr);

        auto outputInfo = ComputeOutputInfo({addNode->output(0)}, layer,
                                            {m_TensorsInfo[inputName].m_info->GetShape(),
                                             m_TensorsInfo[weightName].m_info->GetShape()});

        layer->GetOutputSlot(0).SetTensorInfo(outputInfo[0]);

        RegisterInputSlots(layer, {inputName});
        RegisterOutputSlots(layer, {addNode->output(0)});
    }
    else
    {
        layer = m_Network->AddFullyConnectedLayer(desc, CreateConstTensor(weightName).first, matmulNode.name().c_str());
        BOOST_ASSERT(layer != nullptr);

        auto outputInfo = ComputeOutputInfo({matmulNode.output(0)}, layer,
                                            {m_TensorsInfo[inputName].m_info->GetShape(),
                                             m_TensorsInfo[weightName].m_info->GetShape()});
        layer->GetOutputSlot(0).SetTensorInfo(outputInfo[0]);

        RegisterInputSlots(layer, {inputName});
        RegisterOutputSlots(layer, {matmulNode.output(0)});
    }
}

void OnnxParser::CreateConstantLayer(const std::string& tensorName, const std::string& layerName)
{
    auto armnnTensor = CreateConstTensor(tensorName);

    IConnectableLayer* layer = m_Network->AddConstantLayer(armnnTensor.first, layerName.c_str());
    layer->GetOutputSlot(0).SetTensorInfo(armnnTensor.first.GetInfo());
    RegisterOutputSlots(layer, {tensorName});
}

void OnnxParser::ParseConstant(const onnx::NodeProto& node)
{
    CHECK_VALID_SIZE(static_cast<size_t>(node.attribute_size()), 1);

    if (!node.attribute(0).has_t())
    {
        throw ParseException(boost::str(
              boost::format("Value not found for Constant node '%1%' %2%")
              % node.name()
              % CHECK_LOCATION().AsString()));
    }
    const onnx::TensorProto& onnxTensor = node.attribute(0).t();

    //ONNX can have Float16 and double constant nodes but ArmNN only supports float32
    CHECK_VALID_DATATYPE(node.name(), onnxTensor.name(),
                         static_cast<onnx::TensorProto::DataType>(onnxTensor.data_type()), onnx::TensorProto::FLOAT);

    //Register this as a m_ConstParam so we know we can use it as a constant param in future layers.
    m_TensorsInfo[node.output(0)].m_tensor = std::make_unique<const onnx::TensorProto>(onnxTensor);

    CreateConstantLayer(node.output(0), node.name());

}

void OnnxParser::ParseMaxPool(const onnx::NodeProto& node)
{
    Pooling2dDescriptor desc;
    desc.m_PoolType = PoolingAlgorithm::Max;
    desc.m_PaddingMethod = PaddingMethod::Exclude;
    AddPoolingLayer(node, desc);
}

void OnnxParser::ParseGlobalAveragePool(const onnx::NodeProto& node)
{
    Pooling2dDescriptor desc = Pooling2dDescriptor();
    desc.m_PoolType = PoolingAlgorithm::Average;

    //kernel size is the same as input
    TensorShape inputShape = m_TensorsInfo[node.input(0)].m_info->GetShape();
    desc.m_PoolWidth  = inputShape[3];
    desc.m_PoolHeight = inputShape[2];

    IConnectableLayer* layer = m_Network->AddPooling2dLayer(desc, node.name().c_str());
    BOOST_ASSERT(layer != nullptr);

    auto outputInfo = ComputeOutputInfo({node.output(0)}, layer, {inputShape});
    layer->GetOutputSlot(0).SetTensorInfo(outputInfo[0]);

    // register the input connection slots for the layer, connections are made after all layers have been created
    // only the tensors for the inputs are relevant, exclude the const tensors
    RegisterInputSlots(layer, {node.input(0)});

    // register the output connection slots for the layer, connections are made after all layers have been created
    RegisterOutputSlots(layer, {node.output(0)});
}

void OnnxParser::ParseAveragePool(const onnx::NodeProto& node)
{
    Pooling2dDescriptor desc;
    desc.m_PoolType = PoolingAlgorithm::Average;

    uint32_t count_include_pad = 0;
    count_include_pad = ReadOptionalNodeUint32Attribute(node, "count_include_pad");
    if(count_include_pad) {
        desc.m_PaddingMethod = PaddingMethod::IgnoreValue;
    }
    AddPoolingLayer(node, desc);
}

void OnnxParser::AddPoolingLayer(const onnx::NodeProto& node, Pooling2dDescriptor& desc)
{

    CHECK_VALID_SIZE(static_cast<size_t>(node.input_size()), 1);
    CHECK_VALID_SIZE(static_cast<size_t>(node.output_size()), 1);

    VALID_INPUTS(node, STR_LIST(onnx::TensorProto::FLOAT));

    std::vector<uint32_t> kernel_shape = ReadMandatoryNodeUint32ListAttribute(node, "kernel_shape"); //size of pool win
    std::vector<uint32_t> strides = ReadOptionalNodeUint32ListAttribute(node, "strides");
    std::vector<uint32_t> pads = ReadOptionalNodeUint32ListAttribute(node, "pads");

    desc.m_OutputShapeRounding = OutputShapeRounding::Floor;
    desc.m_PoolWidth  = kernel_shape[1];
    desc.m_PoolHeight = kernel_shape[0];

    if(strides.empty())
    {
        desc.m_StrideX    = 1;
        desc.m_StrideY    = 1;
    }
    else
    {
        desc.m_StrideX    = strides[1];
        desc.m_StrideY    = strides[0];
    }

    //Check new padding version first
    if(pads.empty())
    {
        //Check deprecated version
        std::string paddingString = ReadOptionalNodeStringAttribute(node, "auto_pad");
        if(paddingString != "VALID" && paddingString != "" && paddingString != "NOTSET")
        {
            bool isUpper;
            if( paddingString == "SAME_LOWER")
            {
                isUpper = false;
            }
            else if (paddingString == "SAME_UPPER")
            {
                isUpper = true;
            }
            else
            {
                throw ParseException(boost::str(
                    boost::format("Invalid auto_pad attribute for node %1%. "
                    "Only SAME_UPPER, SAME_LOWER or VALID supported and found %2% %3%")
                    % node.name()
                    % paddingString
                    % CHECK_LOCATION().AsString()));
            }
            auto inputInfo = *m_TensorsInfo[node.input(0)].m_info;
            uint32_t inputHeight = inputInfo.GetShape()[2];
            uint32_t inputWidth  = inputInfo.GetShape()[3];
            CalcPadding(inputHeight, desc.m_PoolHeight, desc.m_StrideY, &desc.m_PadTop, &desc.m_PadBottom, isUpper);
            CalcPadding(inputWidth, desc.m_PoolWidth, desc.m_StrideX, &desc.m_PadLeft, &desc.m_PadRight, isUpper);
        }
    }
    else
    {
        desc.m_PadTop     = pads[0];
        desc.m_PadLeft    = pads[1];
        desc.m_PadBottom  = pads[2];
        desc.m_PadRight   = pads[3];
    }

    IConnectableLayer* layer = m_Network->AddPooling2dLayer(desc, node.name().c_str());
    BOOST_ASSERT(layer != nullptr);

    auto outputInfo = ComputeOutputInfo({node.output(0)}, layer, {m_TensorsInfo[node.input(0)].m_info->GetShape()});
    layer->GetOutputSlot(0).SetTensorInfo(outputInfo[0]);

    // register the input connection slots for the layer, connections are made after all layers have been created
    // only the tensors for the inputs are relevant, exclude the const tensors
    RegisterInputSlots(layer, {node.input(0)});

    // register the output connection slots for the layer, connections are made after all layers have been created
    RegisterOutputSlots(layer, {node.output(0)});
}

void OnnxParser::CreateReshapeLayer(const std::string& inputName,
                                    const std::string& outputName,
                                    const std::string& layerName)
{
    const TensorInfo outputTensorInfo = *m_TensorsInfo[outputName].m_info;
    ReshapeDescriptor reshapeDesc;
    reshapeDesc.m_TargetShape = outputTensorInfo.GetShape();

    IConnectableLayer* layer = m_Network->AddReshapeLayer(reshapeDesc, layerName.c_str());
    BOOST_ASSERT(layer != nullptr);
    layer->GetOutputSlot(0).SetTensorInfo(outputTensorInfo);

    // register the input connection slots for the layer, connections are made after all layers have been created
    // only the tensors for the inputs are relevant, exclude the const tensors
    RegisterInputSlots(layer, {inputName});

    // register the output connection slots for the layer, connections are made after all layers have been created
    RegisterOutputSlots(layer, {outputName});
}

void OnnxParser::ParseReshape(const onnx::NodeProto& node)
{
    CHECK_VALID_SIZE(static_cast<size_t>(node.input_size()), 2);
    CHECK_VALID_SIZE(static_cast<size_t>(node.output_size()), 1);

    CHECK_VALID_DATATYPE(node.name(), node.input(0),
                         m_TensorsInfo[node.input(0)].m_dtype,
                         onnx::TensorProto::FLOAT); //input
    CHECK_VALID_DATATYPE(node.name(), node.input(1),
                         m_TensorsInfo[node.input(1)].m_dtype,
                         onnx::TensorProto::INT64); //shape

    if(!m_TensorsInfo[node.input(1)].isConstant())
    {
        throw ParseException(boost::str(
            boost::format("Shape '%1%' should be constant in Reshape layer '%2%' %3%")
                          % node.input(1)
                          % node.name()
                          % CHECK_LOCATION().AsString()));
    }

    if(m_TensorsInfo[node.input(0)].isConstant())
    {
        //make a new cst tensor -> move the data to the output tensor (the shape is already good in the output tensor)
        if(m_TensorsInfo.count(node.output(0)) == 0)
        {
            m_TensorsInfo[node.output(0)] = OnnxTensor();
        }
        m_TensorsInfo[node.output(0)].m_tensor =
            std::make_unique<onnx::TensorProto>(*m_TensorsInfo[node.input(0)].m_tensor);
    }
    else
    {
        TensorShape inputShape = m_TensorsInfo[node.input(0)].m_info->GetShape();

        if(m_TensorsInfo.count(node.output(0)) == 0 || m_TensorsInfo[node.output(0)].m_info == nullptr)
        {
            auto outInfo = ComputeReshapeInfo(*m_TensorsInfo[node.input(1)].m_tensor, inputShape, node.output(0));
            m_TensorsInfo[node.output(0)].m_info = std::make_unique<TensorInfo>(outInfo);
        }

        CreateReshapeLayer(node.input(0), node.output(0), node.name());
    }
}

void OnnxParser::ParseRelu(const onnx::NodeProto& node)
{
    CHECK_VALID_SIZE(static_cast<size_t>(node.input_size()), 1);
    CHECK_VALID_SIZE(static_cast<size_t>(node.output_size()), 1);

    VALID_INPUTS(node, STR_LIST(onnx::TensorProto::FLOAT));

    ActivationDescriptor desc;
    desc.m_Function = ActivationFunction::ReLu;

    IConnectableLayer* const layer = m_Network->AddActivationLayer(desc, node.name().c_str());
    BOOST_ASSERT(layer != nullptr);

    auto outputInfo = ComputeOutputInfo({ node.output(0)}, layer, {m_TensorsInfo[node.input(0)].m_info->GetShape()});
    layer->GetOutputSlot(0).SetTensorInfo(outputInfo[0]);

    // register the input connection slots for the layer, connections are made after all layers have been created
    // only the tensors for the inputs are relevant, exclude the const tensors
    RegisterInputSlots(layer, {node.input(0)});

    // register the output connection slots for the layer, connections are made after all layers have been created
    RegisterOutputSlots(layer, {node.output(0)});
}


void OnnxParser::AddConvLayerWithDepthwiseConv(const onnx::NodeProto& node, const Convolution2dDescriptor& convDesc)
{
    BOOST_ASSERT(node.op_type() == "Conv");

    DepthwiseConvolution2dDescriptor desc;
    desc.m_PadLeft      = convDesc.m_PadLeft;
    desc.m_PadRight     = convDesc.m_PadRight;
    desc.m_PadTop       = convDesc.m_PadTop;
    desc.m_PadBottom    = convDesc.m_PadBottom;
    desc.m_StrideX      = convDesc.m_StrideX;
    desc.m_StrideY      = convDesc.m_StrideY;
    desc.m_BiasEnabled  = convDesc.m_BiasEnabled;

    armnn::IConnectableLayer* layer;
    auto weightTensor = CreateConstTensor(node.input(1));
    TensorShape& weightShape = weightTensor.first.GetShape();
    weightShape[1] = weightShape[0];
    weightShape[0] = 1;
    m_TensorsInfo[node.input(1)].m_info->SetShape(weightShape);

    if (node.input_size() == 3)
    {
        if(!m_TensorsInfo[node.input(2)].isConstant())
        {
            throw ParseException(boost::str(
                boost::format("Bias '%1%' should be constant in Conv layer '%2%' %3%")
                              % node.input(2)
                              % node.name()
                              % CHECK_LOCATION().AsString()));
        }
        desc.m_BiasEnabled = true;
        auto biasTensor = CreateConstTensor(node.input(2));
        layer = m_Network->AddDepthwiseConvolution2dLayer(desc,
                                                          weightTensor.first,
                                                          biasTensor.first,
                                                          node.name().c_str());
    }
    else
    {
        layer = m_Network->AddDepthwiseConvolution2dLayer(desc,
                                                          weightTensor.first,
                                                          node.name().c_str());
    }
    BOOST_ASSERT(layer != nullptr);

    auto outputInfo = ComputeOutputInfo({ node.output(0) }, layer,
                                        { m_TensorsInfo[node.input(0)].m_info->GetShape(),
                                          m_TensorsInfo[node.input(1)].m_info->GetShape() });

    layer->GetOutputSlot(0).SetTensorInfo(outputInfo[0]);

    // register the input connection slots for the layer, connections are made after all layers have been created
    // only the tensors for the inputs are relevant, exclude the const tensors
    RegisterInputSlots(layer, {node.input(0)});

    // register the output connection slots for the layer, connections are made after all layers have been created
    RegisterOutputSlots(layer, {node.output(0)});
}

void OnnxParser::ParseConv(const onnx::NodeProto& node)
{
    CHECK_VALID_SIZE(static_cast<size_t>(node.input_size()), 2, 3); //input, weight, (bias)
    CHECK_VALID_SIZE(static_cast<size_t>(node.output_size()), 1);

    VALID_INPUTS(node, STR_LIST(onnx::TensorProto::FLOAT));

    if(m_TensorsInfo[node.input(0)].m_info->GetNumDimensions() != 4)
    {
        throw ParseException(boost::str(
            boost::format("ArmNN only supports 2D convolution and Conv layer '%1%' input %2% %3%")
                          % node.name()
                          % TensorInfoAsString(*m_TensorsInfo[node.input(0)].m_info, node.input(0),
                                               m_TensorsInfo[node.input(0)].m_dtype)
                          % CHECK_LOCATION().AsString()));
    }

    if(!m_TensorsInfo[node.input(1)].isConstant())
    {
        throw ParseException(boost::str(
            boost::format("Weights '%1%' should be constant in Conv layer '%2%' %3%")
            % node.input(1)
            % node.name()
            % CHECK_LOCATION().AsString()));
    }

    auto inputInfo = *m_TensorsInfo[node.input(0)].m_info;

    std::vector<uint32_t> dilations = ReadOptionalNodeUint32ListAttribute(node, "dilations");
    if (!dilations.empty())
    {
        std::stringstream ss;
        ss << "[ ";
        for (auto dilation : dilations)
        {
            ss << dilation << ", ";
            if (dilation != 1u)
            {
                ss << "... ]";
                throw ParseException(boost::str(
                    boost::format("ArmNN only supports Convolution layers with dilations [1,1], and node '%1%' "
                                  "has dilatation %2% %3%")
                                   % node.name()
                                   % ss.str()
                                   % CHECK_LOCATION().AsString()));
            }
        }
    }

    Convolution2dDescriptor desc;
    desc.m_BiasEnabled = false;

    std::vector<uint32_t> strides = ReadOptionalNodeUint32ListAttribute(node, "strides");
    if(strides.empty())
    {
        desc.m_StrideX    = 1;
        desc.m_StrideY    = 1;
    }
    else
    {
        desc.m_StrideX    = strides[1];
        desc.m_StrideY    = strides[0];
    }

    std::vector<uint32_t> pads = ReadOptionalNodeUint32ListAttribute(node, "pads");
    //Check new padding version first
    if(pads.empty())
    {
        //Check deprecated version
        std::string paddingString = ReadOptionalNodeStringAttribute(node, "auto_pad");
        if(paddingString != "VALID" && paddingString != "" && paddingString != "NOTSET")
        {
            bool isUpper;
            if( paddingString == "SAME_LOWER")
            {
                isUpper = false;
            }
            else if (paddingString == "SAME_UPPER")
            {
                isUpper = true;
            }
            else
            {
                throw ParseException(boost::str(
                    boost::format("Invalid auto_pad attribute for node %1%. "
                    "Only SAME_UPPER, SAME_LOWER or VALID supported and found %2% %3%")
                    % node.name()
                    % paddingString
                    % CHECK_LOCATION().AsString()));
            }
            uint32_t inputHeight = inputInfo.GetShape()[2];
            uint32_t inputWidth  = inputInfo.GetShape()[3];

            uint32_t weightHeight;
            uint32_t weightWidth;
            std::vector<uint32_t> kernel_shape = ReadOptionalNodeUint32ListAttribute(node, "kernel_shape");
            if (kernel_shape.empty())
            {
                const TensorInfo weightTensorInfo = *m_TensorsInfo[node.input(1)].m_info;
                weightHeight = weightTensorInfo.GetShape()[2];
                weightWidth = weightTensorInfo.GetShape()[3];
            }
            else
            {
                weightHeight = kernel_shape[0];
                weightWidth = kernel_shape[1];
            }
            CalcPadding(inputHeight, weightHeight, desc.m_StrideY, &desc.m_PadTop, &desc.m_PadBottom, isUpper);
            CalcPadding(inputWidth, weightWidth, desc.m_StrideX, &desc.m_PadLeft, &desc.m_PadRight, isUpper);
        }
    }
    else
    {
        desc.m_PadTop     = pads[0];
        desc.m_PadLeft    = pads[1];
        desc.m_PadBottom  = pads[2];
        desc.m_PadRight   = pads[3];
    }

    uint32_t group = ReadOptionalNodeUint32Attribute(node, "group", 1);
    if(group > 1)
    {
        if (group > inputInfo.GetShape()[1])
        {
            throw ParseException(
                boost::str(
                    boost::format(
                        "Error parsing Convolution node: %1%. "
                        "The 'group'=%2% parameter cannot be larger than the "
                        "channel of the input shape=%3% (in NCHW format). %4%") %
                        node.name() %
                        group %
                        inputInfo.GetShape()[1] %
                        CHECK_LOCATION().AsString()));
        }
        else if (group == inputInfo.GetShape()[1])
        {
            // we use a depthwise convolution here, because the number of groups equals to the
            // input channels
            AddConvLayerWithDepthwiseConv(node, desc);
            return;
        }
        else
        {
            // TODO: split the input by channels into channels/groups separate convolutions
            //  and merger the results afterwards
            throw ParseException(boost::str(
                boost::format("Error parsing Convolution node: %1%. "
                "The 'group'=%2% parameter should be 1 or be equal to the "
                "channel of the input shape=%3% (in NCHW format). %4%") %
                node.name() %
                group %
                inputInfo.GetShape()[1] %
                CHECK_LOCATION().AsString()));
        }
    }

    armnn::IConnectableLayer* layer;
    auto weightTensor = CreateConstTensor(node.input(1));

    if (node.input_size() == 3)
    {
        if(!m_TensorsInfo[node.input(2)].isConstant())
        {
            throw ParseException(boost::str(
                boost::format("Bias '%1%' should be constant in Conv layer '%2%' %3%")
                              % node.input(2)
                              % node.name()
                              % CHECK_LOCATION().AsString()));
        }
        desc.m_BiasEnabled = true;
        auto biasTensor = CreateConstTensor(node.input(2));
        layer = m_Network->AddConvolution2dLayer(desc,
                                                 weightTensor.first,
                                                 biasTensor.first,
                                                 node.name().c_str());
    }
    else
    {
        layer = m_Network->AddConvolution2dLayer(desc,
                                                 weightTensor.first,
                                                 node.name().c_str());
    }
    BOOST_ASSERT(layer != nullptr);

    auto outputInfo = ComputeOutputInfo({ node.output(0) }, layer,
                                        { m_TensorsInfo[node.input(0)].m_info->GetShape(),
                                          m_TensorsInfo[node.input(1)].m_info->GetShape() });
    layer->GetOutputSlot(0).SetTensorInfo(outputInfo[0]);

    // register the input connection slots for the layer, connections are made after all layers have been created
    // only the tensors for the inputs are relevant, exclude the const tensors
    RegisterInputSlots(layer, {node.input(0)});

    // register the output connection slots for the layer, connections are made after all layers have been created
    RegisterOutputSlots(layer, {node.output(0)});
}

void OnnxParser::PrependForBroadcast(const std::string& outputName,
                                     const std::string& input0,
                                     const std::string& input1)
{
    //input0 should be reshaped to have same number of dim as input1
    TensorInfo outputTensorInfo = TensorInfo(*m_TensorsInfo[input0].m_info);

    TensorShape input0Shape = m_TensorsInfo[input0].m_info->GetShape();
    TensorShape input1Shape = m_TensorsInfo[input1].m_info->GetShape();

    uint32_t diff = input1Shape.GetNumDimensions() - input0Shape.GetNumDimensions();
    std::vector<uint32_t> newShape;
    while(diff > 0)
    {
        newShape.push_back(1);
        diff--;
    }
    for (uint dim = 0; dim < input0Shape.GetNumDimensions(); ++dim)
    {
        newShape.push_back(input0Shape[dim]);
    }
    outputTensorInfo.SetShape(TensorShape(static_cast<unsigned int>(newShape.size()), newShape.data()));

    //add the new tensor to m_TensorsInfo
    m_TensorsInfo[outputName] = OnnxTensor();
    m_TensorsInfo[outputName].m_info = std::make_unique<TensorInfo>(outputTensorInfo);

    //add reshape layer if the parent was not constant...
    if( ! m_TensorsInfo[input0].isConstant())
    {
        CreateReshapeLayer(input0, outputName, boost::str(boost::format("Add:reshapeOf%1%") % input0));
    }
    else //make it constant and it will be create in Add
    {
        m_TensorsInfo[outputName].m_tensor = std::make_unique<onnx::TensorProto>(*m_TensorsInfo[input0].m_tensor);

    }
}

std::pair<std::string, std::string> OnnxParser::AddPrepareBroadcast(const std::string& input0,
                                                                    const std::string& input1)
{
    std::pair<std::string, std::string> inputs = std::make_pair(input0, input1);

    TensorShape input0Shape = m_TensorsInfo[input0].m_info->GetShape();
    TensorShape input1Shape = m_TensorsInfo[input1].m_info->GetShape();

    if(input1Shape.GetNumDimensions() < input0Shape.GetNumDimensions())
    {
        auto outputName = boost::str(boost::format("reshape_output_%1%") % input1);
        PrependForBroadcast(outputName, input1, input0);
        inputs.second = outputName;
    }
    else if(input0Shape.GetNumDimensions() < input1Shape.GetNumDimensions())
    {
        auto outputName = boost::str(boost::format("reshape_output_%1%") % input0);
        PrependForBroadcast(outputName, input0, input1);
        inputs.first = outputName;
    }
    return inputs;
}

void OnnxParser::ParseAdd(const onnx::NodeProto& node)
{
    CHECK_VALID_SIZE(static_cast<size_t>(node.input_size()), 2);
    CHECK_VALID_SIZE(static_cast<size_t>(node.output_size()), 1);

    VALID_INPUTS(node, STR_LIST(onnx::TensorProto::FLOAT));

     // TODO: unify broadcast validation code across layers
     // tracked by: IVGCVSW-1576

     // Checking broadcast compatibility : only scalar or 1D tensors
     auto inputs = AddPrepareBroadcast(node.input(0), node.input(1));
     auto input0 = *m_TensorsInfo[inputs.first].m_info;
     auto input1 = *m_TensorsInfo[inputs.second].m_info;
     BOOST_ASSERT(input0.GetNumDimensions() == input1.GetNumDimensions());

     unsigned int numDims = input0.GetNumDimensions();
     for (unsigned int i = 0; i < numDims; i++)
     {
         unsigned int dim0 = input0.GetShape()[i];
         unsigned int dim1 = input1.GetShape()[i];
         if (dim0 != dim1 && dim0 != 1 && dim1 != 1)
         {
             throw ParseException(boost::str(
                 boost::format("Broadcast is only supported for scalar or 1D tensors in Add node '%1%'. "
                               "Input dimensions should either match or one should be of size 1 and here, "
                               "%2% and %3% %4%")
                               % node.name()
                               % TensorInfoAsString(*m_TensorsInfo[inputs.first].m_info, inputs.first,
                                                    m_TensorsInfo[inputs.first].m_dtype)
                               % TensorInfoAsString(*m_TensorsInfo[inputs.second].m_info, inputs.second,
                                                    m_TensorsInfo[inputs.second].m_dtype)
                               % CHECK_LOCATION().AsString()));
         }
     }


     IConnectableLayer* layer = m_Network->AddAdditionLayer(node.name().c_str());
     BOOST_ASSERT(layer != nullptr);

     auto outputInfo = ComputeOutputInfo({ node.output(0) }, layer,
                                         { m_TensorsInfo[inputs.first].m_info->GetShape(),
                                           m_TensorsInfo[inputs.second].m_info->GetShape() });
     layer->GetOutputSlot(0).SetTensorInfo(outputInfo[0]);

     // register the input connection -> for constant inputs, we need to make a newDim constant layer
     if(m_TensorsInfo[inputs.first].isConstant()) {

         CreateConstantLayer(inputs.first, boost::str(boost::format("Add:constant_of_%1%") % node.input(0)));
     }
     if(m_TensorsInfo[inputs.second].isConstant()) {

         CreateConstantLayer(inputs.second, boost::str(boost::format("Add:constant_of_%1%") % node.input(1)));
     }
     RegisterInputSlots(layer, {inputs.first, inputs.second});

     // register the output connection
     RegisterOutputSlots(layer, {node.output(0)});
}

void OnnxParser::ParseBatchNormalization(const onnx::NodeProto& node)
{
    //IGNORE momentum parameter and spatial parameters

    CHECK_VALID_SIZE(static_cast<size_t>(node.input_size()), 5);
    CHECK_VALID_SIZE(static_cast<size_t>(node.output_size()), 1);

    VALID_INPUTS(node, STR_LIST(onnx::TensorProto::FLOAT));
    for(int ind = 1; ind < node.input_size(); ++ind)
    {
        auto tensor = node.input(ind);
        if(! m_TensorsInfo[tensor].isConstant())
        {
            throw ParseException(boost::str(
                boost::format("Input tensor '%1%' should be constant in BatchNormalization node '%2%' %3%")
                              % tensor
                              % node.name()
                              % CHECK_LOCATION().AsString()));
        }
    }

    float epsilon = ReadOptionalNodeFloatAttribute(node, "epsilon", 1e-5f);
    BatchNormalizationDescriptor desc;
    desc.m_Eps = epsilon;

    auto scaleTensor = CreateConstTensor(node.input(1));
    auto biasTensor = CreateConstTensor(node.input(2));
    auto meanTensor = CreateConstTensor(node.input(3));
    auto varTensor = CreateConstTensor(node.input(4));

    IConnectableLayer* layer = m_Network->AddBatchNormalizationLayer(desc,
                                                                     meanTensor.first,
                                                                     varTensor.first,
                                                                     biasTensor.first,
                                                                     scaleTensor.first,
                                                                     node.name().c_str());
    BOOST_ASSERT(layer != nullptr);

    auto outputInfo = ComputeOutputInfo({node.output(0)}, layer, {m_TensorsInfo[node.input(0)].m_info->GetShape()});
    layer->GetOutputSlot(0).SetTensorInfo(outputInfo[0]);

    RegisterInputSlots(layer, {node.input(0)}); //don't register constant inputs

    // register the output connection
    RegisterOutputSlots(layer, {node.output(0)});
}

void OnnxParser::SetupInputLayers()
{
    //Find user input and add their layers
    for(int inputIndex = 0; inputIndex < m_Graph->input_size(); ++inputIndex)
    {
        auto input = m_Graph->input(inputIndex);
        if (! m_TensorsInfo[input.name()].isConstant())
        {
            IConnectableLayer* layer =
              m_Network->AddInputLayer(static_cast<armnn::LayerBindingId>(inputIndex), input.name().c_str());
            auto tensorInfo = ToTensorInfo(input);
            layer->GetOutputSlot(0).SetTensorInfo(tensorInfo);

            RegisterOutputSlots(layer,{ input.name() });
        }
    }
}

void OnnxParser::SetupOutputLayers()
{
    if(m_Graph->output_size() == 0)
    {
        throw ParseException(boost::str(boost::format("The given model does not have any outputs %1%")
                                                      % CHECK_LOCATION().AsString()));
    }

    for(int outputIndex = 0; outputIndex < m_Graph->output_size(); ++outputIndex)
    {
        IConnectableLayer* layer =
            m_Network->AddOutputLayer(static_cast<armnn::LayerBindingId>(outputIndex),
                m_Graph->output(outputIndex).name().c_str());

        RegisterInputSlots(layer, { m_Graph->output(outputIndex).name() });
    }
}

void OnnxParser::RegisterInputSlots(IConnectableLayer* layer, const std::vector<std::string>& tensorIds)
{
    BOOST_ASSERT(layer != nullptr);
    if (tensorIds.size() != layer->GetNumInputSlots())
    {
        throw ParseException(
            boost::str(boost::format("The number of tensor inputs (%1%) does not match the number expected (%2%) %3%") %
                       tensorIds.size() %
                       layer->GetNumInputSlots() %
                       CHECK_LOCATION().AsString()));
    }
    for (unsigned int slotIndex = 0; slotIndex < layer->GetNumInputSlots(); ++slotIndex)
    {
        std::string tensorId = tensorIds[slotIndex];
        armnn::IInputSlot* slot = &(layer->GetInputSlot(slotIndex));

        auto it = m_TensorConnections.find(tensorId);

        if (it == m_TensorConnections.end())
        {
            //First time seing this tensor, we need to map it
            m_TensorConnections[tensorId] = TensorSlots();
        }
        m_TensorConnections[tensorId].inputSlots.push_back(slot);
    }
}

void OnnxParser::RegisterOutputSlots(IConnectableLayer* layer, const std::vector<std::string>& tensorIds)
{
    BOOST_ASSERT(layer != nullptr);
    if (tensorIds.size() != layer->GetNumOutputSlots())
    {
        throw ParseException(
            boost::str(boost::format("The number of tensor outputs (%1%) does not match the number expected (%2%) %3% ")
                       % tensorIds.size()
                       % layer->GetNumOutputSlots()
                       % CHECK_LOCATION().AsString()));
    }

    for (unsigned int slotIndex = 0; slotIndex < layer->GetNumOutputSlots(); ++slotIndex)
    {
        std::string tensorId = tensorIds[slotIndex];
        armnn::IOutputSlot* slot = &(layer->GetOutputSlot(slotIndex));

        auto it = m_TensorConnections.find(tensorId);

        if (it == m_TensorConnections.end())
        {
            //First time seing this tensor, we need to map it
            m_TensorConnections[tensorId] = TensorSlots();
        }

        TensorSlots & tensorSlots = m_TensorConnections[tensorId];

        // assuming there is only one producer for that tensor
        if (tensorSlots.outputSlot != nullptr)
        {
            throw ParseException(boost::str(
                    boost::format("Another layer has already registered itself as the producer of "
                                  "tensor:%2% %3%") %
                                   tensorId %
                                   CHECK_LOCATION().AsString()));
        }
        tensorSlots.outputSlot = slot;
    }
}

BindingPointInfo OnnxParser::GetNetworkInputBindingInfo(const std::string& name) const
{
    for(int i = 0; i < m_Graph->input_size(); ++i)
    {
        auto input = m_Graph->input(i);
        if(input.name() == name)
        {
            return std::make_pair(static_cast<armnn::LayerBindingId>(i), ToTensorInfo(input));
        }
    }
    throw InvalidArgumentException(boost::str(boost::format("The input layer '%1%' does not exist %2%")
                                                            % name % CHECK_LOCATION().AsString()));
}

BindingPointInfo OnnxParser::GetNetworkOutputBindingInfo(const std::string& name) const
{
    for(int i = 0; i < m_Graph->output_size(); ++i)
    {
        auto output = m_Graph->output(i);
        if(output.name() == name)
        {
            return std::make_pair(static_cast<armnn::LayerBindingId>(i), ToTensorInfo(output));
        }
    }
    throw InvalidArgumentException(boost::str(boost::format("The output layer '%1%' does not exist %2%")
                                                            % name % CHECK_LOCATION().AsString()));
}

std::vector<std::string> OnnxParser::GetInputs(ModelPtr& model)
{
    if(model == nullptr) {
        throw InvalidArgumentException(boost::str(
            boost::format("The given model cannot be null %1%")
            % CHECK_LOCATION().AsString()));
    }

    std::vector<std::string> inputNames;
    std::map<std::string, bool> isConstant;
    for(auto tensor : model->graph().initializer())
    {
        isConstant[tensor.name()] = true;
    }
    for(auto input : model->graph().input())
    {
        auto it = isConstant.find(input.name());
        if(it == isConstant.end())
        {
            inputNames.push_back(input.name());
        }
    }
    return inputNames;
}

std::vector<std::string> OnnxParser::GetOutputs(ModelPtr& model)
{
    if(model == nullptr) {
        throw InvalidArgumentException(boost::str(
            boost::format("The given model cannot be null %1%")
            % CHECK_LOCATION().AsString()));
    }

    std::vector<std::string> outputNames;
    for(auto output : model->graph().output())
    {
        outputNames.push_back(output.name());
    }
    return outputNames;
}

} // namespace armnnOnnxParser
